#include "video_pipeline.h"

#ifdef USE_ESP32

#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esphome/components/esp_cam_sensor/esp_cam_sensor_camera.h"
#include "esphome/core/log.h"

// Header provided by the `esp_video` external component.
#include "linux/videodev2.h"

namespace esphome {
namespace rtsp_server {

static const char *const TAG = "rtsp_server.video";

/// Only one input buffer is needed: the H.264 encoder is driven synchronously,
/// one access unit at a time, exactly like the ESP-Video reference pipeline.
static constexpr uint32_t ENCODER_OUTPUT_BUFFERS = 1;

static bool set_control(int fd, uint32_t ctrl_class, uint32_t id, int32_t value) {
  struct v4l2_ext_control control[1] = {};
  struct v4l2_ext_controls controls = {};

  control[0].id = id;
  control[0].value = value;
  controls.ctrl_class = ctrl_class;
  controls.count = 1;
  controls.controls = control;

  return ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) == 0;
}

bool VideoPipeline::start(const Config &config, FrameCallback callback) {
  if (this->running_)
    return true;

  this->config_ = config;
  this->callback_ = std::move(callback);

  if (this->camera_ != nullptr) {
    // Frames come from the shared esp_cam_sensor component; it owns the V4L2
    // device, so we must not open it ourselves.
    if (this->config_.codec != VideoCodec::MJPEG) {
      ESP_LOGE(TAG, "'camera_id' only supports 'codec: mjpeg'");
      return false;
    }
    this->camera_->start_streaming();
    this->width_ = this->camera_->get_image_width();
    this->height_ = this->camera_->get_image_height();
    if (!this->open_jpeg_encoder_(this->width_, this->height_))
      return false;
  } else {
    if (!this->open_camera_()) {
      this->close_all_();
      return false;
    }
    if (this->config_.codec == VideoCodec::H264) {
      if (!this->open_h264_encoder_()) {
        this->close_all_();
        return false;
      }
    } else {
      if (!this->open_jpeg_encoder_(this->width_, this->height_)) {
        this->close_all_();
        return false;
      }
      const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (ioctl(this->cam_fd_, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed on the camera (errno %d)", errno);
        this->close_all_();
        return false;
      }
    }
  }

  this->should_stop_ = false;
  this->running_ = true;

  const BaseType_t ok = xTaskCreatePinnedToCore(VideoPipeline::task_trampoline_, "rtsp_video",
                                                this->config_.task_stack, this, this->config_.task_priority,
                                                &this->task_, this->config_.task_core);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "failed to create the video task");
    this->running_ = false;
    this->close_all_();
    return false;
  }

  if (this->config_.codec == VideoCodec::MJPEG) {
    ESP_LOGI(TAG, "pipeline started: %" PRIu32 "x%" PRIu32 " MJPEG q%u @ %" PRIu32 " fps (source: %s)",
             this->width_, this->height_, this->config_.jpeg_quality, this->config_.framerate,
             this->camera_ != nullptr ? "camera component" : this->config_.device.c_str());
  } else {
    ESP_LOGI(TAG, "pipeline started: %" PRIu32 "x%" PRIu32 " H.264 @ %" PRIu32 " fps, %" PRIu32 " bps",
             this->width_, this->height_, this->config_.framerate, this->config_.bitrate);
  }
  return true;
}

void VideoPipeline::stop() {
  if (!this->running_)
    return;

  this->should_stop_ = true;
  // Give the task a chance to leave its DQBUF and unwind on its own.
  for (int i = 0; i < 100 && this->running_; i++)
    vTaskDelay(pdMS_TO_TICKS(10));

  this->close_all_();
}

// ---------------------------------------------------------------------------
// V4L2 capture (headless source)
// ---------------------------------------------------------------------------

bool VideoPipeline::open_camera_() {
  // O_RDONLY matches the ESP-Video reference pipelines; the driver exposes the
  // buffers through mmap/ioctl rather than through read()/write().
  this->cam_fd_ = open(this->config_.device.c_str(), O_RDONLY);
  if (this->cam_fd_ < 0) {
    ESP_LOGE(TAG, "cannot open '%s' (errno %d). Is the `esp_video` component configured and the sensor detected?",
             this->config_.device.c_str(), errno);
    return false;
  }

  // The MIPI-CSI device cannot rescale: the resolution comes from the sensor
  // format that was selected at init time. Read it back and adapt.
  struct v4l2_format format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->cam_fd_, VIDIOC_G_FMT, &format) != 0) {
    ESP_LOGE(TAG, "VIDIOC_G_FMT failed (errno %d)", errno);
    return false;
  }
  this->width_ = format.fmt.pix.width;
  this->height_ = format.fmt.pix.height;

  const bool h264 = this->config_.codec == VideoCodec::H264;

  if (h264 && ((this->width_ % 16) != 0 || (this->height_ % 16) != 0)) {
    ESP_LOGE(TAG,
             "sensor resolution %" PRIu32 "x%" PRIu32 " is not a multiple of 16; the hardware H.264 encoder "
             "cannot use it. Pick another sensor format, or use 'codec: mjpeg'.",
             this->width_, this->height_);
    return false;
  }
  if (!h264 && ((this->width_ % 8) != 0 || (this->height_ % 8) != 0 || this->width_ > 2040 ||
                this->height_ > 2040)) {
    ESP_LOGE(TAG,
             "sensor resolution %" PRIu32 "x%" PRIu32 " cannot be described by RFC 2435: it must be a multiple "
             "of 8 and at most 2040 pixels on each side.",
             this->width_, this->height_);
    return false;
  }

  // H.264 consumes planar YUV420; the JPEG engine takes the ISP's RGB565 output.
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = this->width_;
  format.fmt.pix.height = this->height_;
  format.fmt.pix.pixelformat = h264 ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_RGB565;
  if (ioctl(this->cam_fd_, VIDIOC_S_FMT, &format) != 0) {
    ESP_LOGE(TAG, "the camera cannot output %s (errno %d). Enable the ISP in the `esp_video` component.",
             h264 ? "YUV420" : "RGB565", errno);
    return false;
  }

  // Best effort: ask the sensor to slow down instead of dropping frames later.
  struct v4l2_streamparm parm = {};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = this->config_.framerate;
  if (ioctl(this->cam_fd_, VIDIOC_S_PARM, &parm) != 0)
    ESP_LOGD(TAG, "VIDIOC_S_PARM unsupported, falling back to software frame dropping");

  this->apply_camera_controls_();

  struct v4l2_requestbuffers req = {};
  req.count = this->config_.buffer_count;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(this->cam_fd_, VIDIOC_REQBUFS, &req) != 0) {
    ESP_LOGE(TAG, "VIDIOC_REQBUFS failed on the camera (errno %d)", errno);
    return false;
  }

  for (uint8_t i = 0; i < this->config_.buffer_count; i++) {
    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (ioctl(this->cam_fd_, VIDIOC_QUERYBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed for buffer %u (errno %d)", i, errno);
      return false;
    }

    this->cam_buffers_[i] = static_cast<uint8_t *>(
        mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->cam_fd_, buf.m.offset));
    if (this->cam_buffers_[i] == nullptr || this->cam_buffers_[i] == MAP_FAILED) {
      ESP_LOGE(TAG, "mmap failed for buffer %u (%" PRIu32 " bytes)", i, static_cast<uint32_t>(buf.length));
      this->cam_buffers_[i] = nullptr;
      return false;
    }
    this->cam_buffer_size_[i] = buf.length;

    if (ioctl(this->cam_fd_, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QBUF failed for buffer %u (errno %d)", i, errno);
      return false;
    }
  }

  return true;
}

void VideoPipeline::apply_camera_controls_() {
  if (this->config_.vflip && !set_control(this->cam_fd_, V4L2_CTRL_CLASS_USER, V4L2_CID_VFLIP, 1))
    ESP_LOGW(TAG, "the sensor does not support V4L2_CID_VFLIP");
  if (this->config_.hflip && !set_control(this->cam_fd_, V4L2_CTRL_CLASS_USER, V4L2_CID_HFLIP, 1))
    ESP_LOGW(TAG, "the sensor does not support V4L2_CID_HFLIP");
}

// ---------------------------------------------------------------------------
// Hardware JPEG encoder
// ---------------------------------------------------------------------------

bool VideoPipeline::open_jpeg_encoder_(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    ESP_LOGE(TAG, "cannot start the JPEG encoder: the frame size is unknown");
    return false;
  }

  jpeg_encode_engine_cfg_t cfg = {};
  cfg.timeout_ms = 2000;
  jpeg_encoder_handle_t handle = nullptr;
  const esp_err_t err = jpeg_new_encoder_engine(&cfg, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
    return false;
  }
  this->jpeg_encoder_ = handle;

  // The output must come from jpeg_alloc_encoder_mem. RGB565 input size is a
  // safe upper bound: a JPEG that large would mean the encoder failed anyway.
  const size_t need = static_cast<size_t>(width) * height * 2;
  jpeg_encode_memory_alloc_cfg_t mem_cfg = {};
  mem_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  size_t got = 0;
  this->jpeg_out_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(need, &mem_cfg, &got));
  if (this->jpeg_out_ == nullptr) {
    ESP_LOGE(TAG, "failed to allocate %u bytes for the JPEG output", static_cast<unsigned>(need));
    return false;
  }
  this->jpeg_out_size_ = got != 0 ? got : need;

  return true;
}

bool VideoPipeline::encode_jpeg_(const uint8_t *src, uint32_t width, uint32_t height, uint32_t timestamp) {
  jpeg_encode_cfg_t cfg = {};
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;  // RFC 2435 type 1
  cfg.image_quality = this->config_.jpeg_quality;
  cfg.width = static_cast<uint32_t>(width);
  cfg.height = static_cast<uint32_t>(height);

  uint32_t encoded = 0;
  const esp_err_t err =
      jpeg_encoder_process(static_cast<jpeg_encoder_handle_t>(this->jpeg_encoder_), &cfg,
                           const_cast<uint8_t *>(src), static_cast<size_t>(width) * height * 2, this->jpeg_out_,
                           this->jpeg_out_size_, &encoded);
  if (err != ESP_OK || encoded == 0) {
    ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
    return false;
  }

  if (this->callback_)
    this->callback_(this->jpeg_out_, encoded, timestamp);
  this->frames_encoded_++;
  return true;
}

// ---------------------------------------------------------------------------
// Hardware H.264 encoder (V4L2 memory-to-memory)
// ---------------------------------------------------------------------------

bool VideoPipeline::open_h264_encoder_() {
  this->enc_fd_ = open(this->config_.encoder_device.c_str(), O_RDONLY);
  if (this->enc_fd_ < 0) {
    ESP_LOGE(TAG,
             "cannot open '%s' (errno %d). Set `enable_h264: true` on the `esp_video` component so the hardware "
             "encoder device is built.",
             this->config_.encoder_device.c_str(), errno);
    return false;
  }

  // The OUTPUT queue is the encoder input (raw YUV420) and must be configured
  // before the CAPTURE queue, which validates its geometry against it.
  struct v4l2_format format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  format.fmt.pix.width = this->width_;
  format.fmt.pix.height = this->height_;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
  if (ioctl(this->enc_fd_, VIDIOC_S_FMT, &format) != 0) {
    ESP_LOGE(TAG, "VIDIOC_S_FMT (OUTPUT/YUV420) failed on the encoder (errno %d)", errno);
    return false;
  }

  struct v4l2_requestbuffers req = {};
  req.count = ENCODER_OUTPUT_BUFFERS;
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  req.memory = V4L2_MEMORY_USERPTR;  // fed directly with the camera's buffers
  if (ioctl(this->enc_fd_, VIDIOC_REQBUFS, &req) != 0) {
    ESP_LOGE(TAG, "VIDIOC_REQBUFS (OUTPUT) failed on the encoder (errno %d)", errno);
    return false;
  }

  format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = this->width_;
  format.fmt.pix.height = this->height_;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
  if (ioctl(this->enc_fd_, VIDIOC_S_FMT, &format) != 0) {
    ESP_LOGE(TAG, "VIDIOC_S_FMT (CAPTURE/H264) failed on the encoder (errno %d)", errno);
    return false;
  }

  req = {};
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(this->enc_fd_, VIDIOC_REQBUFS, &req) != 0) {
    ESP_LOGE(TAG, "VIDIOC_REQBUFS (CAPTURE) failed on the encoder (errno %d)", errno);
    return false;
  }

  struct v4l2_buffer buf = {};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;
  if (ioctl(this->enc_fd_, VIDIOC_QUERYBUF, &buf) != 0) {
    ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed on the encoder (errno %d)", errno);
    return false;
  }

  this->enc_capture_buffer_ = static_cast<uint8_t *>(
      mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->enc_fd_, buf.m.offset));
  if (this->enc_capture_buffer_ == nullptr || this->enc_capture_buffer_ == MAP_FAILED) {
    ESP_LOGE(TAG, "mmap failed for the encoder output buffer");
    this->enc_capture_buffer_ = nullptr;
    return false;
  }
  this->enc_capture_size_ = buf.length;

  if (ioctl(this->enc_fd_, VIDIOC_QBUF, &buf) != 0) {
    ESP_LOGE(TAG, "VIDIOC_QBUF failed on the encoder (errno %d)", errno);
    return false;
  }

  this->apply_encoder_controls_();

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->enc_fd_, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "VIDIOC_STREAMON (CAPTURE) failed on the encoder (errno %d)", errno);
    return false;
  }
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(this->enc_fd_, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "VIDIOC_STREAMON (OUTPUT) failed on the encoder (errno %d)", errno);
    return false;
  }
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->cam_fd_, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "VIDIOC_STREAMON failed on the camera (errno %d)", errno);
    return false;
  }

  return true;
}

void VideoPipeline::apply_encoder_controls_() {
  if (!set_control(this->enc_fd_, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_BITRATE,
                   static_cast<int32_t>(this->config_.bitrate)))
    ESP_LOGW(TAG, "failed to set the H.264 bitrate");
  if (!set_control(this->enc_fd_, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                   static_cast<int32_t>(this->config_.gop)))
    ESP_LOGW(TAG, "failed to set the H.264 I-frame period");
  if (!set_control(this->enc_fd_, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
                   static_cast<int32_t>(this->config_.min_qp)))
    ESP_LOGW(TAG, "failed to set the H.264 minimum QP");
  if (!set_control(this->enc_fd_, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                   static_cast<int32_t>(this->config_.max_qp)))
    ESP_LOGW(TAG, "failed to set the H.264 maximum QP");
}

// ---------------------------------------------------------------------------
// Pipeline task
// ---------------------------------------------------------------------------

void VideoPipeline::task_trampoline_(void *arg) {
  static_cast<VideoPipeline *>(arg)->run_();
}

void VideoPipeline::run_() {
  if (this->camera_ != nullptr) {
    this->run_camera_source_();
  } else {
    this->run_v4l2_source_();
  }

  this->running_ = false;
  this->task_ = nullptr;
  vTaskDelete(nullptr);
}

void VideoPipeline::run_camera_source_() {
  const uint32_t frame_ms = 1000 / (this->config_.framerate > 0 ? this->config_.framerate : 15);
  auto *camera = this->camera_;

  while (!this->should_stop_) {
    const int64_t started_us = esp_timer_get_time();

    // Only one consumer may dequeue from V4L2; `drive_camera: false` leaves that
    // to lvgl_camera_display and we simply read whatever the latest frame is.
    if (this->config_.drive_camera && !camera->capture_frame()) {
      vTaskDelay(pdMS_TO_TICKS(frame_ms));
      continue;
    }

    esp_cam_sensor::SimpleBufferElement *element = nullptr;
    uint8_t *rgb = nullptr;
    int width = 0;
    int height = 0;
    if (!camera->get_current_rgb_frame(&element, &rgb, &width, &height) || rgb == nullptr || width <= 0 ||
        height <= 0) {
      vTaskDelay(pdMS_TO_TICKS(frame_ms));
      continue;
    }

    // The camera's own buffer is DMA-capable PSRAM, so the JPEG engine can read
    // it in place; it must stay held until the encode returns.
    const uint32_t timestamp = static_cast<uint32_t>((started_us * 9) / 100);
    this->encode_jpeg_(rgb, static_cast<uint32_t>(width), static_cast<uint32_t>(height), timestamp);
    camera->release_buffer(element);

    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (elapsed_ms < frame_ms) {
      vTaskDelay(pdMS_TO_TICKS(frame_ms - elapsed_ms));
    } else {
      taskYIELD();
    }
  }
}

void VideoPipeline::run_v4l2_source_() {
  const int64_t frame_interval_us = 1000000LL / static_cast<int64_t>(this->config_.framerate);
  int64_t next_frame_us = 0;

  while (!this->should_stop_) {
    struct v4l2_buffer cam_buf = {};
    cam_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cam_buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(this->cam_fd_, VIDIOC_DQBUF, &cam_buf) != 0) {
      ESP_LOGW(TAG, "VIDIOC_DQBUF failed on the camera (errno %d)", errno);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    const int64_t now_us = esp_timer_get_time();

    // Software frame pacing: if the sensor runs faster than the configured
    // framerate, requeue the buffer immediately without spending encoder time.
    if (now_us < next_frame_us) {
      ioctl(this->cam_fd_, VIDIOC_QBUF, &cam_buf);
      this->frames_dropped_++;
      continue;
    }
    next_frame_us = now_us + frame_interval_us;

    const uint32_t timestamp = static_cast<uint32_t>((now_us * 9) / 100);

    if (this->config_.codec == VideoCodec::MJPEG) {
      this->encode_jpeg_(this->cam_buffers_[cam_buf.index], this->width_, this->height_, timestamp);
      ioctl(this->cam_fd_, VIDIOC_QBUF, &cam_buf);
      continue;
    }

    // Hand the camera buffer straight to the H.264 encoder input queue.
    struct v4l2_buffer enc_out = {};
    enc_out.index = 0;
    enc_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    enc_out.memory = V4L2_MEMORY_USERPTR;
    enc_out.m.userptr = reinterpret_cast<unsigned long>(this->cam_buffers_[cam_buf.index]);
    enc_out.length = cam_buf.bytesused;
    if (ioctl(this->enc_fd_, VIDIOC_QBUF, &enc_out) != 0) {
      ESP_LOGW(TAG, "VIDIOC_QBUF failed on the encoder input (errno %d)", errno);
      ioctl(this->cam_fd_, VIDIOC_QBUF, &cam_buf);
      continue;
    }

    struct v4l2_buffer enc_cap = {};
    enc_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    enc_cap.memory = V4L2_MEMORY_MMAP;
    const bool encoded = ioctl(this->enc_fd_, VIDIOC_DQBUF, &enc_cap) == 0;
    if (!encoded)
      ESP_LOGW(TAG, "VIDIOC_DQBUF failed on the encoder output (errno %d)", errno);

    // The encode is done, so the camera buffer can go back into rotation.
    ioctl(this->cam_fd_, VIDIOC_QBUF, &cam_buf);
    ioctl(this->enc_fd_, VIDIOC_DQBUF, &enc_out);

    if (encoded) {
      if (this->callback_)
        this->callback_(this->enc_capture_buffer_, enc_cap.bytesused, timestamp);
      this->frames_encoded_++;

      enc_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      enc_cap.memory = V4L2_MEMORY_MMAP;
      ioctl(this->enc_fd_, VIDIOC_QBUF, &enc_cap);
    }
  }
}

void VideoPipeline::close_all_() {
  int type;

  if (this->cam_fd_ >= 0) {
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(this->cam_fd_, VIDIOC_STREAMOFF, &type);
  }
  if (this->enc_fd_ >= 0) {
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(this->enc_fd_, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(this->enc_fd_, VIDIOC_STREAMOFF, &type);
  }

  for (uint8_t i = 0; i < 4; i++) {
    if (this->cam_buffers_[i] != nullptr) {
      munmap(this->cam_buffers_[i], this->cam_buffer_size_[i]);
      this->cam_buffers_[i] = nullptr;
      this->cam_buffer_size_[i] = 0;
    }
  }

  if (this->enc_capture_buffer_ != nullptr) {
    munmap(this->enc_capture_buffer_, this->enc_capture_size_);
    this->enc_capture_buffer_ = nullptr;
    this->enc_capture_size_ = 0;
  }

  if (this->jpeg_encoder_ != nullptr) {
    jpeg_del_encoder_engine(static_cast<jpeg_encoder_handle_t>(this->jpeg_encoder_));
    this->jpeg_encoder_ = nullptr;
  }
  if (this->jpeg_out_ != nullptr) {
    heap_caps_free(this->jpeg_out_);
    this->jpeg_out_ = nullptr;
    this->jpeg_out_size_ = 0;
  }

  if (this->enc_fd_ >= 0) {
    close(this->enc_fd_);
    this->enc_fd_ = -1;
  }
  if (this->cam_fd_ >= 0) {
    close(this->cam_fd_);
    this->cam_fd_ = -1;
  }
}

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
