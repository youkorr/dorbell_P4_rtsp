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
    /// How long the microphone stays muted after the far end's audio has
    /// actually finished coming out of the speaker -- not after the last packet
    /// arrived, which is a jitter buffer earlier.
    ///
    /// This is the room's reverberation time, nothing else. Too short and the
    /// microphone reopens while the far end's own voice is still ringing in the
    /// room, sends it back, and every word comes home as an echo. 300 ms was
    /// that mistake; 500 ms suits a normal entrance hall. A bare, hard-walled
    /// stairwell may want 800 ms or more -- at the cost of how quickly the
    /// visitor can interrupt.
    uint32_t talk_timeout_ms{500};

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

  // ---- metering -----------------------------------------------------------
  // "I cannot tell whether the microphone works" is the hardest fault to place
  // in this chain, because every stage between the capsule and the browser can
  // swallow the sound silently. These readings are taken at the two points that
  // matter -- just after capture and just before playback -- so the device can
  // answer the question on its own.

  /// Peak level of the last capture window, 0.0 – 1.0, gain applied. Decays
  /// over ~1 s so a template sensor polling every second sees speech peaks.
  float mic_level() const;
  /// The same reading in dBFS: -100.0 is digital silence, 0.0 is full scale.
  /// Speech at a sensible level sits between -30 and -6 dBFS.
  float mic_level_db() const;
  /// Peak of what the speaker ACCEPTED, 0.0 – 1.0. A sink that refuses the data
  /// reads as silence here, which is the whole point: the meter has to fall when
  /// nothing comes out, not when nothing is offered.
  float speaker_level() const;
  float speaker_level_db() const;

  /// Bytes handed to the speaker, and bytes it actually took. Equal means the
  /// output path is healthy. `written` frozen at 0 while `offered` climbs means
  /// the sink is refusing everything -- a stopped speaker component, or one that
  /// never started -- and no amount of volume will produce a sound.
  uint32_t speaker_bytes_offered() const { return this->speaker_bytes_offered_; }
  uint32_t speaker_bytes_written() const { return this->speaker_bytes_written_; }
  /// Number of short writes: the speaker took less than it was given.
  uint32_t speaker_drops() const { return this->speaker_drops_; }
  /// True when the speaker has taken everything it was recently offered.
  bool speaker_healthy() const;
  /// A fixed-width text meter for the logs, e.g. "[####------]".
  const char *mic_level_bar() const;
  const char *speaker_level_bar() const;

  /// Loudest level seen over a long window (about a minute), in dBFS.
  ///
  /// `mic_level_db()` decays in a second, which is right for a live meter but
  /// useless in a status block printed every ten: unless you happen to be
  /// speaking at that exact instant, it reports room tone and the microphone
  /// looks dead. This one answers the question that actually matters -- "what
  /// is the loudest thing this microphone has heard recently" -- so a single
  /// log line is enough to tell a quiet room from a deaf microphone.
  float mic_peak_hold_db() const;
  float speaker_peak_hold_db() const;

  /// PCM samples read from the microphone since boot. Stuck at 0 (or simply
  /// stuck) means the source delivers nothing -- a different fault from a source
  /// that delivers silence, and one no amount of gain will fix.
  uint32_t mic_samples() const { return this->mic_samples_; }
  /// True while the microphone source keeps delivering samples.
  bool mic_alive() const;

  /// Local monitoring: send the microphone straight to the speaker. Speak, hear
  /// yourself, and both halves of the audio path are proven at once -- without
  /// the network, go2rtc or a browser.
  ///
  /// It TIMES OUT on purpose, and that is not a limitation. Microphone and
  /// speaker sit on the same board: with any useful gain the loop is acoustic,
  /// it runs away to full scale, and what you get is a howl rather than your
  /// voice. Left on, it also holds the speaker against the real backchannel.
  /// There is no legitimate reason to leave it running, and every reason not to
  /// -- so it stops on its own.
  void set_loopback(bool enabled);
  /// False once the monitor has timed out, so a switch bound to this reads back
  /// the truth rather than what it was last set to.
  bool loopback() const;

  /// Queue a beep on the speaker, to prove the output path by itself.
  void play_test_tone(uint32_t frequency, uint32_t duration_ms);

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

  /// Record the peak of a PCM block into a decaying meter. Returns that block's
  /// own peak, so the caller can feed it to the long-hold meter too.
  static uint32_t update_peak_(volatile uint32_t *peak, volatile int64_t *stamp, const int16_t *pcm,
                               size_t samples);
  /// Fold one block peak into a meter that holds for about a minute.
  static void update_hold_(volatile uint32_t *hold, volatile int64_t *since, uint32_t block_peak);
  /// Read a decaying meter back as 0.0 – 1.0.
  static float read_peak_(volatile uint32_t peak, volatile int64_t stamp);
  /// Fill `dst` (>= 13 bytes) with a "[####------]" meter for `level`.
  static void render_bar_(char *dst, float level);
  /// Record one write to the sink: how much was offered, how much it took.
  void account_write_(const int16_t *src, size_t offered_bytes, size_t written_bytes, size_t bytes_per_sample);
  /// Push every byte to the ESPHome speaker, re-offering what it did not take.
  /// Returns the total accepted, which is short only if the deadline expired.
  size_t push_all_(const uint8_t *data, size_t len);
  /// Append one buffer's worth of test tone to the playback path, if pending.
  size_t take_test_tone_(int16_t *dst, size_t samples);

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
  /// When a backchannel packet last arrived from the network.
  volatile int64_t last_playback_us_{0};
  /// When far-end audio was last actually handed to the speaker. Later than the
  /// above by the depth of the jitter buffer; see is_talking().
  volatile int64_t last_speaker_write_us_{0};

  volatile uint32_t packets_sent_{0};
  volatile uint32_t packets_received_{0};
  volatile uint32_t packets_dropped_{0};

  /// Peak meters, held as plain 32-bit words so any task can read them without
  /// a lock: the writer is a single task and a torn read is not possible.
  volatile uint32_t mic_peak_{0};
  volatile int64_t mic_peak_us_{0};
  volatile uint32_t speaker_peak_{0};
  volatile int64_t speaker_peak_us_{0};

  /// Same meters over a long window, so a status line printed every ten seconds
  /// still reports the loudest sound of the last minute rather than whatever
  /// happened to be there at that instant.
  volatile uint32_t mic_hold_{0};
  volatile int64_t mic_hold_since_us_{0};
  volatile uint32_t speaker_hold_{0};
  volatile int64_t speaker_hold_since_us_{0};

  volatile uint32_t mic_samples_{0};
  volatile int64_t mic_last_sample_us_{0};

  volatile uint32_t speaker_bytes_offered_{0};
  volatile uint32_t speaker_bytes_written_{0};
  volatile uint32_t speaker_drops_{0};
  bool speaker_restart_logged_{false};
  bool loopback_yield_logged_{false};

  volatile bool loopback_{false};
  volatile int64_t loopback_since_us_{0};
  /// Remaining samples of the pending beep, and its phase state.
  volatile uint32_t tone_remaining_{0};
  uint32_t tone_frequency_{1000};
  uint32_t tone_phase_{0};

  /// Rendered lazily by mic_level_bar() / speaker_level_bar(), which are only
  /// ever called from the logging path.
  mutable char mic_bar_[16]{};
  mutable char speaker_bar_[16]{};

  uint32_t rtp_timestamp_{0};
};

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
