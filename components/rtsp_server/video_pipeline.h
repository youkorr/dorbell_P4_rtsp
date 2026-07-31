#pragma once

// Pulls in USE_ESP32 before it is tested below.
#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {

// Forward declared so this header does not drag in the esp_cam_sensor headers;
// the camera source is optional.
namespace esp_cam_sensor {
class MipiDSICamComponent;
}  // namespace esp_cam_sensor

namespace rtsp_server {

/// Camera -> MJPEG pipeline.
///
/// Two frame sources:
///   * an `esp_cam_sensor` camera component, whose RGB565 buffers are shared
///     with LVGL, so a board with a screen can preview and stream at once;
///   * the `esp_video` V4L2 device directly, for a headless build.
///
/// One encoder: the ESP32-P4 hardware JPEG engine. The camera buffer is handed
/// to it untouched, so nothing is copied.
///
/// H.264 is deliberately not offered. The P4's hardware encoder would produce
/// it, but nothing on the device can decode it back for the LVGL preview, and
/// the encoder wants YUV420 while LVGL wants RGB565 -- so the local preview and
/// the stream could not share one camera. MJPEG keeps a single RGB565 frame
/// feeding both, which is the whole point of this component.
class VideoPipeline {
 public:
  struct Config {
    std::string device{"/dev/video0"};
    uint32_t framerate{15};
    uint8_t jpeg_quality{25};
    uint8_t buffer_count{3};
    bool vflip{false};
    bool hflip{false};
    /// When a camera component is shared with LVGL, only one consumer may drive
    /// the V4L2 dequeue; set false to let `lvgl_camera_display` do it.
    bool drive_camera{true};
    uint32_t task_stack{6144};
    uint8_t task_priority{5};
    int task_core{1};
  };

  /// Called from the pipeline task for every encoded frame. The buffer is only
  /// valid for the duration of the call.
  using FrameCallback = std::function<void(const uint8_t *frame, size_t len, uint32_t timestamp_90k)>;

  void set_camera(esp_cam_sensor::MipiDSICamComponent *camera) { this->camera_ = camera; }

  bool start(const Config &config, FrameCallback callback);
  void stop();

  /// Hand the V4L2 dequeue back and forth at runtime.
  ///
  /// Exactly one consumer may dequeue from the shared camera. When an LVGL
  /// preview is on screen it is the natural one, and this pipeline just reads
  /// the latest frame. But the preview can be switched off -- and then nobody
  /// dequeues, and the RTSP stream freezes on its last picture with no error
  /// anywhere. Flipping this from the preview's on/off action keeps exactly one
  /// consumer driving the camera in both states.
  void set_drive_camera(bool drive) { this->drive_camera_ = drive; }
  bool drive_camera() const { return this->drive_camera_; }

  bool is_running() const { return this->running_; }
  uint32_t width() const { return this->width_; }
  uint32_t height() const { return this->height_; }
  uint32_t frames_encoded() const { return this->frames_encoded_; }
  uint32_t frames_dropped() const { return this->frames_dropped_; }

 protected:
  static void task_trampoline_(void *arg);
  void run_();
  void run_camera_source_();
  void run_v4l2_source_();

  bool open_camera_();
  bool open_jpeg_encoder_(uint32_t width, uint32_t height);
  void close_all_();
  void apply_camera_controls_();

  /// Encode one RGB565 frame with the hardware JPEG engine and hand it to the
  /// callback. `src` must be DMA-capable memory (camera buffers always are).
  bool encode_jpeg_(const uint8_t *src, uint32_t width, uint32_t height, uint32_t timestamp);

  Config config_;
  FrameCallback callback_;

  esp_cam_sensor::MipiDSICamComponent *camera_{nullptr};

  int cam_fd_{-1};
  uint8_t *cam_buffers_[4]{nullptr, nullptr, nullptr, nullptr};
  size_t cam_buffer_size_[4]{0, 0, 0, 0};

  /// `jpeg_encoder_handle_t`, kept opaque so this header stays driver-free.
  void *jpeg_encoder_{nullptr};
  uint8_t *jpeg_out_{nullptr};
  size_t jpeg_out_size_{0};

  uint32_t width_{0};
  uint32_t height_{0};

  TaskHandle_t task_{nullptr};
  volatile bool running_{false};
  volatile bool should_stop_{false};
  /// Mirrors `config_.drive_camera`, but volatile: the video task reads it on
  /// every frame and another task may flip it at any moment.
  volatile bool drive_camera_{true};

  volatile uint32_t frames_encoded_{0};
  volatile uint32_t frames_dropped_{0};
};

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
