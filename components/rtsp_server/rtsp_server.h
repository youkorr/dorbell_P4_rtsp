#pragma once

// Pulls in USE_ESP32 before it is tested below.
#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <memory>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "audio_pipeline.h"
#include "rtp.h"
#include "video_pipeline.h"

namespace esphome {
namespace rtsp_server {

/// One connected RTSP client.
struct RtspSession {
  int fd{-1};
  std::string rx;               ///< bytes received but not yet consumed
  std::string session_id;       ///< RTSP Session header value
  bool authenticated{false};
  bool playing{false};
  bool wants_backchannel{false};

  /// Per-stream setup state and the interleaved channel the client asked for.
  bool setup[3]{false, false, false};
  uint8_t channel[3]{0, 2, 4};

  uint32_t last_activity_ms{0};
};

/// RTSP server with ONVIF backchannel support, designed to be ingested by
/// go2rtc and re-published as WebRTC.
///
/// Transport is RTP interleaved over the RTSP TCP connection (RFC 2326 §10.12)
/// only. That is go2rtc's default, it needs no extra ports, and it is what makes
/// the backchannel work over a single socket.
class RTSPServer : public Component, public RtpSender {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // ---- configuration (called from generated code) -------------------------
  void set_port(uint16_t port) { this->port_ = port; }
  void set_path(const std::string &path) { this->path_ = path; }
  void set_credentials(const std::string &user, const std::string &password);
  void set_max_clients(uint8_t max_clients) { this->max_clients_ = max_clients; }
  void set_packet_size(uint16_t size) { this->packet_size_ = size; }
  void set_video_config(const VideoPipeline::Config &config) {
    this->video_config_ = config;
    this->video_enabled_ = true;
  }
  void set_audio_config(const AudioPipeline::Config &config) {
    this->audio_config_ = config;
    this->audio_enabled_ = true;
  }
  void set_camera(esp_cam_sensor::MipiDSICamComponent *camera) { this->video_.set_camera(camera); }
  /// Take over (or hand back) the V4L2 dequeue at runtime -- call this from
  /// whatever turns the LVGL preview on and off, so the camera always has
  /// exactly one consumer driving it. See VideoPipeline::set_drive_camera.
  void set_drive_camera(bool drive) { this->video_.set_drive_camera(drive); }
  bool drive_camera() const { return this->video_.drive_camera(); }
  void set_microphone(microphone::Microphone *microphone) { this->audio_.set_microphone(microphone); }
  void set_speaker(speaker::Speaker *speaker) { this->audio_.set_speaker(speaker); }

  void add_on_client_connected_callback(std::function<void()> &&cb) {
    this->client_connected_callback_.add(std::move(cb));
  }
  void add_on_client_disconnected_callback(std::function<void()> &&cb) {
    this->client_disconnected_callback_.add(std::move(cb));
  }
  void add_on_talk_start_callback(std::function<void()> &&cb) { this->talk_start_callback_.add(std::move(cb)); }
  void add_on_talk_end_callback(std::function<void()> &&cb) { this->talk_end_callback_.add(std::move(cb)); }

  // ---- state, usable from lambdas -----------------------------------------
  /// Full RTSP URL of this stream, e.g. "rtsp://192.168.1.50:8554/doorbell".
  /// Handy for a text_sensor or an on-screen label.
  std::string stream_url() const;
  uint8_t client_count() const { return this->client_count_; }
  bool is_streaming() const { return this->active_streams_ > 0; }
  bool is_talking() const { return this->audio_.is_talking(); }

  // ---- audio diagnostics, usable from lambdas ------------------------------
  // These exist so "is the microphone working?" can be answered from Home
  // Assistant or from the screen, without a scope and without go2rtc. They
  // read live counters and levels, so they are cheap enough to poll every
  // second from a template sensor.

  /// True once the capture/playback tasks are up.
  bool audio_running() const { return this->audio_.is_running(); }
  /// True when a speaker is wired, i.e. when the backchannel can be announced.
  bool has_speaker() const { return this->audio_.has_speaker(); }
  /// Peak level of the microphone over the last window, 0.0 – 1.0, gain applied.
  float mic_level() const { return this->audio_.mic_level(); }
  /// Same reading in dBFS: -100.0 for digital silence, 0.0 for full scale.
  float mic_level_db() const { return this->audio_.mic_level_db(); }
  /// Peak level of what is being pushed to the speaker, 0.0 – 1.0.
  float speaker_level() const { return this->audio_.speaker_level(); }
  float speaker_level_db() const { return this->audio_.speaker_level_db(); }
  /// PCM samples read from the microphone since boot. Frozen at 0 means the
  /// source delivers nothing at all -- a different fault from "delivers silence".
  uint32_t mic_samples() const { return this->audio_.mic_samples(); }
  /// True while the microphone source keeps delivering samples.
  bool mic_alive() const { return this->audio_.mic_alive(); }
  uint32_t audio_packets_sent() const { return this->audio_.packets_sent(); }
  uint32_t audio_packets_received() const { return this->audio_.packets_received(); }
  uint32_t audio_packets_dropped() const { return this->audio_.packets_dropped(); }

  /// Route the microphone straight to the speaker, locally. Speak and you hear
  /// yourself: one press proves capture, companding and playback at once, with
  /// no network, no go2rtc and no browser in the way.
  void set_audio_loopback(bool enabled) { this->audio_.set_loopback(enabled); }
  bool audio_loopback() const { return this->audio_.loopback(); }
  /// Play a beep on the speaker. Proves the output path on its own.
  void play_test_tone(uint32_t frequency, uint32_t duration_ms) {
    this->audio_.play_test_tone(frequency, duration_ms);
  }

  // ---- RtpSender ----------------------------------------------------------
  void send_rtp(StreamKind kind, uint8_t *buf, size_t rtp_len) override;

 protected:
  static void network_task_trampoline_(void *arg);
  void network_run_();

  bool start_pipelines_();
  bool open_listener_();
  void accept_client_();
  void close_session_(size_t index);
  void service_session_(RtspSession &session);
  void drain_tx_ring_();

  // RTSP protocol
  void handle_request_(RtspSession &session, const std::string &request);
  void handle_interleaved_(RtspSession &session, uint8_t channel, const uint8_t *payload, size_t len);
  bool check_auth_(const RtspSession &session, const std::string &request) const;
  void send_response_(RtspSession &session, const std::string &response);
  void send_simple_(RtspSession &session, int code, const char *reason, int cseq, const std::string &extra = "");
  bool send_all_(int fd, const uint8_t *data, size_t len);

  std::string build_sdp_(const std::string &local_ip, bool with_backchannel);
  std::string local_ip_of_(int fd) const;

  void on_video_frame_(const uint8_t *jpeg, size_t len, uint32_t timestamp);
  void on_audio_frame_(const uint8_t *g711, size_t count, uint32_t timestamp);

  // ---- configuration ------------------------------------------------------
  uint16_t port_{8554};
  std::string path_{"/doorbell"};
  std::string auth_token_;  ///< pre-computed "Basic <base64>" value, empty = open
  uint8_t max_clients_{2};
  uint16_t packet_size_{1400};

  bool video_enabled_{false};
  bool audio_enabled_{false};
  VideoPipeline::Config video_config_;
  AudioPipeline::Config audio_config_;

  // ---- runtime ------------------------------------------------------------
  VideoPipeline video_;
  AudioPipeline audio_;

  std::unique_ptr<MjpegPacketizer> mjpeg_packetizer_;
  std::unique_ptr<G711Packetizer> audio_packetizer_;

  int listen_fd_{-1};
  std::vector<std::unique_ptr<RtspSession>> sessions_;
  TaskHandle_t network_task_{nullptr};
  RingbufHandle_t tx_ring_{nullptr};

  bool pipelines_started_{false};
  uint32_t session_counter_{0};

  volatile uint8_t client_count_{0};
  volatile uint8_t active_streams_{0};
  volatile uint32_t tx_overflows_{0};
  volatile uint32_t tx_frames_dropped_{0};
  /// Client-to-server RTP arriving while no backchannel is set up. Rate-limits
  /// the warning that would otherwise repeat on every packet.
  uint32_t stray_rtp_{0};
  /// Tracks the last PLAYing client set up: bit0 video, bit1 audio, bit2
  /// backchannel. A single byte so the main loop can read it without racing
  /// against the network task, which owns `sessions_`.
  volatile uint8_t negotiated_mask_{0};
  uint32_t last_status_ms_{0};

  /// Periodic one-block summary of the whole chain, for diagnosis.
  void log_status_();
  /// Recompute `negotiated_mask_` across all playing sessions. Network task only.
  void refresh_negotiated_mask_();
  /// Set when a video frame could not be queued whole: the rest of that frame
  /// is discarded so the receiver never sees a fragment with a hole in it.
  bool tx_dropping_frame_{false};
  volatile uint32_t jpeg_parse_errors_{0};

  // Edge detection performed on the main loop so triggers never fire from a
  // FreeRTOS task.
  uint8_t last_reported_clients_{0};
  bool last_reported_talking_{false};

  CallbackManager<void()> client_connected_callback_;
  CallbackManager<void()> client_disconnected_callback_;
  CallbackManager<void()> talk_start_callback_;
  CallbackManager<void()> talk_end_callback_;
};

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
