#pragma once

// Pulls in USE_ESP32 before it is tested below.
#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cstdint>
#include <functional>
#include <vector>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

namespace esphome {

// Forward declared so this header does not drag in the audio component headers;
// the ESPHome microphone/speaker source is optional.
namespace microphone {
class Microphone;
}  // namespace microphone
namespace speaker {
class Speaker;
}  // namespace speaker

namespace rtsp_server {

/// G.711 always runs at 8 kHz on the wire, whatever the I2S clock is.
static constexpr uint32_t G711_SAMPLE_RATE = 8000;
/// One RTP packet every 20 ms, i.e. 160 G.711 bytes.
static constexpr size_t G711_SAMPLES_PER_PACKET = G711_SAMPLE_RATE / 50;

enum class AudioCodec : uint8_t {
  PCMU = 0,  ///< RTP payload type 0
  PCMA = 8,  ///< RTP payload type 8
};

enum class MicMode : uint8_t {
  STD,  ///< Standard I2S (Philips), e.g. INMP441 / ICS-43434
  PDM,  ///< PDM microphone
};

/// Bidirectional I2S audio, companded to G.711 for RTP.
///
/// The capture side reads the microphone, optionally decimates to 8 kHz,
/// compands to G.711 and pushes 20 ms packets to a callback. The playback side
/// drains G.711 bytes fed by the RTSP backchannel, expands them and writes them
/// to the speaker.
class AudioPipeline {
 public:
  struct Config {
    AudioCodec codec{AudioCodec::PCMU};
    uint32_t sample_rate{16000};  ///< I2S clock rate; 8000 or 16000

    int mic_port{0};
    MicMode mic_mode{MicMode::STD};
    int mic_bclk{-1};
    int mic_lrclk{-1};
    int mic_din{-1};
    uint8_t mic_bits{32};
    bool mic_right_slot{false};
    float mic_gain{4.0f};

    bool speaker_enabled{true};
    /// Set to the same value as `mic_port` to run one port in full duplex,
    /// in which case the speaker reuses the microphone's BCLK/LRCLK.
    int speaker_port{1};
    int speaker_bclk{-1};
    int speaker_lrclk{-1};
    int speaker_dout{-1};
    uint8_t speaker_bits{16};
    bool speaker_right_slot{false};
    float speaker_volume{0.8f};

    /// Mute the microphone while the far end is talking, to stop the speaker
    /// from feeding back into the microphone. There is no echo canceller.
    bool half_duplex{true};
    /// How long after the last backchannel packet the call is considered over.
    uint32_t talk_timeout_ms{300};

    uint32_t task_stack{4096};
    uint8_t task_priority{6};
    int task_core{0};
  };

  /// Called from the capture task with one 20 ms G.711 packet.
  using AudioCallback = std::function<void(const uint8_t *g711, size_t count, uint32_t timestamp_8k)>;

  /// Use an ESPHome `microphone` platform instead of raw I2S pins. This is what
  /// lets the doorbell share a codec (fdaudio / ES8311+ES7210, i2s_audio, ...)
  /// with voice assistant or any other consumer.
  void set_microphone(microphone::Microphone *microphone) { this->external_mic_ = microphone; }
  /// Use an ESPHome `speaker` platform instead of raw I2S pins.
  void set_speaker(speaker::Speaker *speaker) { this->external_speaker_ = speaker; }
  bool uses_external_audio() const { return this->external_mic_ != nullptr; }

  bool start(const Config &config, AudioCallback callback);
  void stop();

  /// Queue G.711 bytes received on the RTSP backchannel for playback.
  void play_g711(const uint8_t *data, size_t len);

  bool is_running() const { return this->running_; }
  bool has_speaker() const { return this->tx_handle_ != nullptr || this->external_speaker_ != nullptr; }
  /// True while backchannel audio is actively being played.
  bool is_talking() const;
  uint32_t packets_sent() const { return this->packets_sent_; }
  uint32_t packets_received() const { return this->packets_received_; }
  uint32_t packets_dropped() const { return this->packets_dropped_; }

 protected:
  static void capture_task_trampoline_(void *arg);
  static void playback_task_trampoline_(void *arg);
  void capture_run_();
  void playback_run_();

  bool init_i2s_();
  void deinit_i2s_();
  uint8_t payload_type_() const { return static_cast<uint8_t>(this->config_.codec); }
  uint8_t compand_(int16_t pcm) const;
  int16_t expand_(uint8_t g711) const;

  /// Read up to `samples` 16-bit mono samples from whichever source is active.
  size_t read_pcm_(int16_t *dst, size_t samples);
  /// Push 16-bit mono samples to whichever sink is active.
  void write_pcm_(const int16_t *src, size_t samples);
  void on_external_mic_data_(const std::vector<uint8_t> &data);

  Config config_;
  AudioCallback callback_;

  microphone::Microphone *external_mic_{nullptr};
  speaker::Speaker *external_speaker_{nullptr};
  /// PCM straight off the ESPHome microphone callback, drained by the capture
  /// task. Unused when the raw I2S source is active.
  StreamBufferHandle_t capture_buffer_{nullptr};

  i2s_chan_handle_t rx_handle_{nullptr};
  i2s_chan_handle_t tx_handle_{nullptr};

  StreamBufferHandle_t playback_buffer_{nullptr};

  TaskHandle_t capture_task_{nullptr};
  TaskHandle_t playback_task_{nullptr};

  volatile bool running_{false};
  volatile bool should_stop_{false};
  volatile int64_t last_playback_us_{0};

  volatile uint32_t packets_sent_{0};
  volatile uint32_t packets_received_{0};
  volatile uint32_t packets_dropped_{0};

  uint32_t rtp_timestamp_{0};
};

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
