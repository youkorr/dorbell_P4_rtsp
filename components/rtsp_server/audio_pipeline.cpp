#include "audio_pipeline.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include "esp_err.h"
#include "esp_timer.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/log.h"
#include "g711.h"
#include "soc/soc_caps.h"

#if SOC_I2S_SUPPORTS_PDM_RX
#include "driver/i2s_pdm.h"
#endif

namespace esphome {
namespace rtsp_server {

static const char *const TAG = "rtsp_server.audio";

/// 500 ms of G.711 is plenty of jitter buffer on a LAN, and bounds the latency
/// that can build up if the network delivers a burst.
static constexpr size_t PLAYBACK_BUFFER_BYTES = G711_SAMPLE_RATE / 2;

/// How long a peak is held before it is considered stale. Long enough that a
/// sensor polling once a second still catches the peak of a spoken word, short
/// enough that the meter falls back to silence while you watch it.
static constexpr int64_t PEAK_HOLD_US = 1000000;
/// The microphone is declared stopped after this long without a single sample.
static constexpr int64_t MIC_ALIVE_US = 2000000;
/// Reported instead of -inf dBFS, so a sensor never has to carry an infinity.
static constexpr float SILENCE_DBFS = -100.0f;
/// Window of the long-hold meters: long enough that a status line printed every
/// ten seconds still catches a spoken word.
static constexpr int64_t PEAK_HOLD_LONG_US = 60000000;

/// Saturate to the 16-bit PCM range.
///
/// Written out rather than with std::min/std::max on purpose: on riscv32 an
/// int32_t is a `long int`, so bare literals and the value deduce to different
/// template parameters and the call does not compile.
static inline int16_t clamp_pcm16(int32_t value) {
  if (value > 32767)
    return 32767;
  if (value < -32768)
    return -32768;
  return static_cast<int16_t>(value);
}

static i2s_slot_bit_width_t slot_width(uint8_t bits) {
  return bits == 16 ? I2S_SLOT_BIT_WIDTH_16BIT : I2S_SLOT_BIT_WIDTH_32BIT;
}

static i2s_data_bit_width_t data_width(uint8_t bits) {
  return bits == 16 ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_32BIT;
}

bool AudioPipeline::start(const Config &config, AudioCallback callback) {
  if (this->running_)
    return true;

  this->config_ = config;
  this->callback_ = std::move(callback);

  this->playback_buffer_ = xStreamBufferCreate(PLAYBACK_BUFFER_BYTES, 1);
  if (this->playback_buffer_ == nullptr) {
    ESP_LOGE(TAG, "failed to allocate the playback buffer");
    return false;
  }

  if (this->external_mic_ != nullptr) {
    // Half a second of 16-bit mono at the codec's rate.
    this->capture_buffer_ = xStreamBufferCreate(this->config_.sample_rate, 1);
    if (this->capture_buffer_ == nullptr) {
      ESP_LOGE(TAG, "failed to allocate the capture buffer");
      vStreamBufferDelete(this->playback_buffer_);
      this->playback_buffer_ = nullptr;
      return false;
    }

    this->external_mic_->add_data_callback(
        [this](const std::vector<uint8_t> &data) { this->on_external_mic_data_(data); });
    if (!this->external_mic_->is_running())
      this->external_mic_->start();

    if (this->external_speaker_ != nullptr) {
      this->external_speaker_->set_audio_stream_info(
          audio::AudioStreamInfo(16, 1, static_cast<uint32_t>(this->config_.sample_rate)));
      this->external_speaker_->start();
    }
  } else if (!this->init_i2s_()) {
    this->deinit_i2s_();
    vStreamBufferDelete(this->playback_buffer_);
    this->playback_buffer_ = nullptr;
    return false;
  }

  this->should_stop_ = false;
  this->running_ = true;

  if (xTaskCreatePinnedToCore(AudioPipeline::capture_task_trampoline_, "rtsp_mic", this->config_.task_stack, this,
                              this->config_.task_priority, &this->capture_task_,
                              this->config_.task_core) != pdPASS) {
    ESP_LOGE(TAG, "failed to create the capture task");
    this->running_ = false;
    this->deinit_i2s_();
    return false;
  }

  if (this->has_speaker() &&
      xTaskCreatePinnedToCore(AudioPipeline::playback_task_trampoline_, "rtsp_spk", this->config_.task_stack, this,
                              this->config_.task_priority, &this->playback_task_,
                              this->config_.task_core) != pdPASS) {
    ESP_LOGE(TAG, "failed to create the playback task");
  }

  ESP_LOGI(TAG, "audio started: %s @ 8 kHz on the wire, PCM at %" PRIu32 " Hz (%s), speaker %s",
           this->config_.codec == AudioCodec::PCMU ? "PCMU" : "PCMA", this->config_.sample_rate,
           this->external_mic_ != nullptr ? "ESPHome microphone/speaker" : "direct I2S",
           this->has_speaker() ? "enabled" : "disabled");
  return true;
}

void AudioPipeline::on_external_mic_data_(const std::vector<uint8_t> &data) {
  if (this->capture_buffer_ == nullptr || data.empty())
    return;
  // Never block the audio component's own task: if the RTSP side is behind,
  // dropping is better than stalling a microphone that other consumers share.
  xStreamBufferSend(this->capture_buffer_, data.data(), data.size(), 0);
}

size_t AudioPipeline::read_pcm_(int16_t *dst, size_t samples) {
  if (this->external_mic_ != nullptr) {
    const size_t got = xStreamBufferReceive(this->capture_buffer_, dst, samples * sizeof(int16_t),
                                            pdMS_TO_TICKS(200));
    return got / sizeof(int16_t);
  }

  const size_t bytes_per_sample = this->config_.mic_bits / 8;
  std::vector<uint8_t> raw(samples * bytes_per_sample);

  size_t bytes_read = 0;
  const esp_err_t err = i2s_channel_read(this->rx_handle_, raw.data(), raw.size(), &bytes_read,
                                         pdMS_TO_TICKS(200));
  if (err != ESP_OK || bytes_read == 0) {
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
      ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
    return 0;
  }

  const size_t got = bytes_read / bytes_per_sample;
  // Normalise to 16-bit PCM. A 32-bit slot carries the sample MSB-justified
  // (24-bit parts such as the INMP441), so the top 16 bits are what we want.
  if (bytes_per_sample == 4) {
    const int32_t *src = reinterpret_cast<const int32_t *>(raw.data());
    for (size_t i = 0; i < got; i++)
      dst[i] = static_cast<int16_t>(src[i] >> 16);
  } else {
    std::memcpy(dst, raw.data(), got * sizeof(int16_t));
  }
  return got;
}

void AudioPipeline::account_write_(const int16_t *src, size_t offered_bytes, size_t written_bytes,
                                   size_t bytes_per_sample) {
  this->speaker_bytes_offered_ += static_cast<uint32_t>(offered_bytes);
  this->speaker_bytes_written_ += static_cast<uint32_t>(written_bytes);

  if (written_bytes < offered_bytes) {
    this->speaker_drops_++;
    if ((this->speaker_drops_ % 50) == 1) {
      ESP_LOGW(TAG,
               "the speaker accepted %u of %u bytes. It is not keeping up, or it is not running: "
               "%" PRIu32 " short writes so far.",
               static_cast<unsigned>(written_bytes), static_cast<unsigned>(offered_bytes),
               this->speaker_drops_);
    }
  }

  // The meter reflects what the speaker ACCEPTED, never what we offered it.
  // Metering the offer was actively misleading: with a sink that refuses
  // everything, the bar sat at a healthy level while nothing came out -- the
  // one reading that had to be trustworthy said the opposite of the truth.
  if (written_bytes == 0)
    return;
  const size_t accepted_samples = written_bytes / (bytes_per_sample == 0 ? 1 : bytes_per_sample);
  this->update_hold_(&this->speaker_hold_, &this->speaker_hold_since_us_,
                     update_peak_(&this->speaker_peak_, &this->speaker_peak_us_, src, accepted_samples));
}

void AudioPipeline::write_pcm_(const int16_t *src, size_t samples) {
  if (samples == 0)
    return;

  if (this->external_speaker_ != nullptr) {
    // An ESPHome speaker may stop itself when it has been idle, and a stopped
    // speaker silently returns 0 from every play(). Restart it rather than
    // spend the rest of the session writing into nothing.
    if (!this->external_speaker_->is_running()) {
      this->external_speaker_->start();
      if (!this->speaker_restart_logged_) {
        ESP_LOGW(TAG, "the speaker component had stopped; restarting it");
        this->speaker_restart_logged_ = true;
      }
    }

    const size_t offered = samples * sizeof(int16_t);
    // Wait briefly rather than drop on a momentarily full ring buffer: 20 ms is
    // one packet, so this bounds the latency to what we would have added anyway.
    const size_t written =
        this->external_speaker_->play(reinterpret_cast<const uint8_t *>(src), offered, pdMS_TO_TICKS(20));
    this->account_write_(src, offered, written, sizeof(int16_t));
    return;
  }

  if (this->tx_handle_ == nullptr)
    return;

  const size_t bytes_per_sample = this->config_.speaker_bits / 8;
  if (bytes_per_sample == 2) {
    size_t written = 0;
    i2s_channel_write(this->tx_handle_, src, samples * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
    this->account_write_(src, samples * sizeof(int16_t), written, sizeof(int16_t));
    return;
  }

  std::vector<int32_t> wide(samples);
  for (size_t i = 0; i < samples; i++)
    wide[i] = static_cast<int32_t>(src[i]) << 16;
  size_t written = 0;
  i2s_channel_write(this->tx_handle_, wide.data(), wide.size() * sizeof(int32_t), &written, pdMS_TO_TICKS(200));
  this->account_write_(src, wide.size() * sizeof(int32_t), written, sizeof(int32_t));
}

void AudioPipeline::stop() {
  if (!this->running_)
    return;

  this->should_stop_ = true;
  for (int i = 0; i < 100 && (this->capture_task_ != nullptr || this->playback_task_ != nullptr); i++)
    vTaskDelay(pdMS_TO_TICKS(10));

  this->running_ = false;

  if (this->external_speaker_ != nullptr)
    this->external_speaker_->stop();
  if (this->external_mic_ != nullptr && this->external_mic_->is_running())
    this->external_mic_->stop();

  this->deinit_i2s_();

  if (this->playback_buffer_ != nullptr) {
    vStreamBufferDelete(this->playback_buffer_);
    this->playback_buffer_ = nullptr;
  }
  if (this->capture_buffer_ != nullptr) {
    vStreamBufferDelete(this->capture_buffer_);
    this->capture_buffer_ = nullptr;
  }
}

bool AudioPipeline::init_i2s_() {
  const bool full_duplex = this->config_.speaker_enabled && this->config_.speaker_port == this->config_.mic_port;

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(this->config_.mic_port), I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chan_cfg, full_duplex ? &this->tx_handle_ : nullptr, &this->rx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed on port %d: %s", this->config_.mic_port, esp_err_to_name(err));
    return false;
  }

  // ---- Microphone ----------------------------------------------------------
  // The configuration structs are filled field by field rather than with
  // designated initializers so the code keeps compiling across the ESP-IDF
  // releases that reshuffled these headers.
  if (this->config_.mic_mode == MicMode::PDM) {
#if SOC_I2S_SUPPORTS_PDM_RX
    i2s_pdm_rx_clk_config_t pdm_clk = I2S_PDM_RX_CLK_DEFAULT_CONFIG(this->config_.sample_rate);
    i2s_pdm_rx_slot_config_t pdm_slot =
        I2S_PDM_RX_SLOT_DEFAULT_CONFIG(data_width(this->config_.mic_bits), I2S_SLOT_MODE_MONO);
    pdm_slot.slot_mask = this->config_.mic_right_slot ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;

    i2s_pdm_rx_config_t pdm_cfg = {};
    pdm_cfg.clk_cfg = pdm_clk;
    pdm_cfg.slot_cfg = pdm_slot;
    pdm_cfg.gpio_cfg.clk = static_cast<gpio_num_t>(this->config_.mic_lrclk);
    pdm_cfg.gpio_cfg.din = static_cast<gpio_num_t>(this->config_.mic_din);
    pdm_cfg.gpio_cfg.invert_flags.clk_inv = false;

    err = i2s_channel_init_pdm_rx_mode(this->rx_handle_, &pdm_cfg);
#else
    ESP_LOGE(TAG, "this chip has no PDM receiver; use `mode: std`");
    return false;
#endif
  } else {
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(this->config_.sample_rate);
    i2s_std_slot_config_t slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(data_width(this->config_.mic_bits), I2S_SLOT_MODE_MONO);
    slot.slot_mask = this->config_.mic_right_slot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    slot.slot_bit_width = slot_width(this->config_.mic_bits);

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = clk;
    std_cfg.slot_cfg = slot;
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(this->config_.mic_bclk);
    std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(this->config_.mic_lrclk);
    std_cfg.gpio_cfg.dout = full_duplex ? static_cast<gpio_num_t>(this->config_.speaker_dout) : I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = static_cast<gpio_num_t>(this->config_.mic_din);
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(this->rx_handle_, &std_cfg);
    if (err == ESP_OK && full_duplex) {
      // In full duplex both channels share the clock and slot layout; only the
      // slot mask may differ.
      std_cfg.slot_cfg.slot_mask = this->config_.speaker_right_slot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
      err = i2s_channel_init_std_mode(this->tx_handle_, &std_cfg);
    }
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to configure the microphone channel: %s", esp_err_to_name(err));
    return false;
  }

  // ---- Speaker on its own port --------------------------------------------
  if (this->config_.speaker_enabled && !full_duplex) {
    i2s_chan_config_t tx_chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(this->config_.speaker_port), I2S_ROLE_MASTER);
    tx_chan_cfg.auto_clear = true;

    err = i2s_new_channel(&tx_chan_cfg, &this->tx_handle_, nullptr);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "i2s_new_channel failed on port %d: %s", this->config_.speaker_port, esp_err_to_name(err));
      this->tx_handle_ = nullptr;
    } else {
      i2s_std_clk_config_t spk_clk = I2S_STD_CLK_DEFAULT_CONFIG(this->config_.sample_rate);
      i2s_std_slot_config_t spk_slot =
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(data_width(this->config_.speaker_bits), I2S_SLOT_MODE_MONO);
      spk_slot.slot_mask = this->config_.speaker_right_slot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
      spk_slot.slot_bit_width = slot_width(this->config_.speaker_bits);

      i2s_std_config_t spk_cfg = {};
      spk_cfg.clk_cfg = spk_clk;
      spk_cfg.slot_cfg = spk_slot;
      spk_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
      spk_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(this->config_.speaker_bclk);
      spk_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(this->config_.speaker_lrclk);
      spk_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(this->config_.speaker_dout);
      spk_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
      spk_cfg.gpio_cfg.invert_flags.mclk_inv = false;
      spk_cfg.gpio_cfg.invert_flags.bclk_inv = false;
      spk_cfg.gpio_cfg.invert_flags.ws_inv = false;

      err = i2s_channel_init_std_mode(this->tx_handle_, &spk_cfg);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure the speaker channel: %s", esp_err_to_name(err));
        i2s_del_channel(this->tx_handle_);
        this->tx_handle_ = nullptr;
      }
    }
  }

  if (i2s_channel_enable(this->rx_handle_) != ESP_OK) {
    ESP_LOGE(TAG, "failed to enable the microphone channel");
    return false;
  }
  if (this->tx_handle_ != nullptr && i2s_channel_enable(this->tx_handle_) != ESP_OK) {
    ESP_LOGE(TAG, "failed to enable the speaker channel");
    i2s_del_channel(this->tx_handle_);
    this->tx_handle_ = nullptr;
  }

  return true;
}

void AudioPipeline::deinit_i2s_() {
  if (this->rx_handle_ != nullptr) {
    i2s_channel_disable(this->rx_handle_);
    i2s_del_channel(this->rx_handle_);
    this->rx_handle_ = nullptr;
  }
  if (this->tx_handle_ != nullptr) {
    i2s_channel_disable(this->tx_handle_);
    i2s_del_channel(this->tx_handle_);
    this->tx_handle_ = nullptr;
  }
}

uint8_t AudioPipeline::compand_(int16_t pcm) const {
  return this->config_.codec == AudioCodec::PCMU ? g711::linear_to_ulaw(pcm) : g711::linear_to_alaw(pcm);
}

int16_t AudioPipeline::expand_(uint8_t value) const {
  return this->config_.codec == AudioCodec::PCMU ? g711::ulaw_to_linear(value) : g711::alaw_to_linear(value);
}

// ---------------------------------------------------------------------------
// Metering
// ---------------------------------------------------------------------------

/// Deliberately lock-free. Each meter has one writer in practice, and the only
/// consequence of a race would be a meter reading one block out of date -- not
/// worth a mutex on the audio path.
void AudioPipeline::update_hold_(volatile uint32_t *hold, volatile int64_t *since, uint32_t block_peak) {
  const int64_t now = esp_timer_get_time();
  if (*since == 0 || (now - *since) > PEAK_HOLD_LONG_US) {
    *hold = block_peak;
    *since = now;
    return;
  }
  if (block_peak > *hold)
    *hold = block_peak;
}

uint32_t AudioPipeline::update_peak_(volatile uint32_t *peak, volatile int64_t *stamp, const int16_t *pcm,
                                     size_t samples) {
  uint32_t block_peak = 0;
  for (size_t i = 0; i < samples; i++) {
    // -32768 has no positive counterpart in int16_t, so widen before negating.
    const int32_t value = pcm[i];
    const uint32_t magnitude = static_cast<uint32_t>(value < 0 ? -value : value);
    if (magnitude > block_peak)
      block_peak = magnitude;
  }

  const int64_t now = esp_timer_get_time();
  // Hold the loudest value seen in the window, then start a fresh window. A
  // decaying maximum is what makes the reading legible: an instantaneous peak
  // of a 20 ms block is mostly zero even while someone is speaking.
  if (block_peak >= *peak || (now - *stamp) > PEAK_HOLD_US) {
    *peak = block_peak;
    *stamp = now;
  }
  return block_peak;
}

float AudioPipeline::read_peak_(volatile uint32_t peak, volatile int64_t stamp) {
  if (stamp == 0 || (esp_timer_get_time() - stamp) > PEAK_HOLD_US)
    return 0.0f;
  return static_cast<float>(peak) / 32768.0f;
}

void AudioPipeline::render_bar_(char *dst, float level) {
  // Logarithmic, because a linear meter spends nine tenths of its travel on the
  // top 20 dB and shows nothing at all for ordinary speech.
  const float db = level <= 0.0f ? SILENCE_DBFS : 20.0f * std::log10(level);
  int filled = static_cast<int>((db + 60.0f) / 6.0f);  // -60 dBFS .. 0 dBFS
  if (filled < 0)
    filled = 0;
  if (filled > 10)
    filled = 10;

  dst[0] = '[';
  for (int i = 0; i < 10; i++)
    dst[1 + i] = i < filled ? '#' : '-';
  dst[11] = ']';
  dst[12] = '\0';
}

float AudioPipeline::mic_level() const { return read_peak_(this->mic_peak_, this->mic_peak_us_); }

float AudioPipeline::speaker_level() const { return read_peak_(this->speaker_peak_, this->speaker_peak_us_); }

float AudioPipeline::mic_level_db() const {
  const float level = this->mic_level();
  return level <= 0.0f ? SILENCE_DBFS : 20.0f * std::log10(level);
}

float AudioPipeline::speaker_level_db() const {
  const float level = this->speaker_level();
  return level <= 0.0f ? SILENCE_DBFS : 20.0f * std::log10(level);
}

const char *AudioPipeline::mic_level_bar() const {
  render_bar_(this->mic_bar_, this->mic_level());
  return this->mic_bar_;
}

const char *AudioPipeline::speaker_level_bar() const {
  render_bar_(this->speaker_bar_, this->speaker_level());
  return this->speaker_bar_;
}

/// A held peak does not decay, so it is read straight rather than through
/// read_peak_ -- the whole point is that it survives the quiet moments.
static float hold_to_db(uint32_t hold) {
  if (hold == 0)
    return SILENCE_DBFS;
  return 20.0f * std::log10(static_cast<float>(hold) / 32768.0f);
}

float AudioPipeline::mic_peak_hold_db() const { return hold_to_db(this->mic_hold_); }

float AudioPipeline::speaker_peak_hold_db() const { return hold_to_db(this->speaker_hold_); }

bool AudioPipeline::speaker_healthy() const {
  if (!this->has_speaker())
    return false;
  // Nothing offered yet is not a fault: it just means nobody has spoken and no
  // test tone has been played.
  if (this->speaker_bytes_offered_ == 0)
    return true;
  return this->speaker_bytes_written_ == this->speaker_bytes_offered_;
}

bool AudioPipeline::mic_alive() const {
  if (!this->running_ || this->mic_last_sample_us_ == 0)
    return false;
  return (esp_timer_get_time() - this->mic_last_sample_us_) < MIC_ALIVE_US;
}

void AudioPipeline::play_test_tone(uint32_t frequency, uint32_t duration_ms) {
  if (!this->has_speaker())
    return;
  this->tone_frequency_ = frequency == 0 ? 1000 : frequency;
  this->tone_phase_ = 0;
  // Written last: the playback task reads this to decide whether to synthesise,
  // so the frequency must already be in place when it becomes non-zero.
  this->tone_remaining_ = (this->config_.sample_rate * duration_ms) / 1000;
}

size_t AudioPipeline::take_test_tone_(int16_t *dst, size_t samples) {
  const uint32_t remaining = this->tone_remaining_;
  if (remaining == 0)
    return 0;

  const size_t count = remaining < samples ? remaining : samples;
  const uint32_t rate = this->config_.sample_rate;
  for (size_t i = 0; i < count; i++) {
    const float phase = 2.0f * 3.14159265f * static_cast<float>(this->tone_phase_) *
                        static_cast<float>(this->tone_frequency_) / static_cast<float>(rate);
    // Half scale: loud enough to hear across a room, quiet enough not to clip
    // an amplifier that is already turned up.
    dst[i] = static_cast<int16_t>(std::sin(phase) * 16000.0f);
    // Wrapping at one second keeps the sample index small, so the float never
    // loses precision on a long beep. The frequency is a whole number of hertz,
    // so a full second is a whole number of cycles and the wrap is seamless.
    if (++this->tone_phase_ >= rate)
      this->tone_phase_ = 0;
  }
  this->tone_remaining_ = remaining - static_cast<uint32_t>(count);
  return count;
}

bool AudioPipeline::is_talking() const {
  if (this->last_playback_us_ == 0)
    return false;
  return (esp_timer_get_time() - this->last_playback_us_) < (this->config_.talk_timeout_ms * 1000LL);
}

void AudioPipeline::play_g711(const uint8_t *data, size_t len) {
  if (this->playback_buffer_ == nullptr || !this->has_speaker() || len == 0)
    return;

  this->packets_received_++;
  this->last_playback_us_ = esp_timer_get_time();

  // Never block the network task: if the jitter buffer is full the far end is
  // ahead of the speaker and dropping is the right answer.
  const size_t written = xStreamBufferSend(this->playback_buffer_, data, len, 0);
  if (written < len)
    this->packets_dropped_++;
}

void AudioPipeline::capture_task_trampoline_(void *arg) {
  static_cast<AudioPipeline *>(arg)->capture_run_();
}

void AudioPipeline::playback_task_trampoline_(void *arg) {
  static_cast<AudioPipeline *>(arg)->playback_run_();
}

void AudioPipeline::capture_run_() {
  const uint32_t decimation = this->config_.sample_rate / G711_SAMPLE_RATE;  // 1 or 2
  const size_t pcm_samples = G711_SAMPLES_PER_PACKET * decimation;

  std::vector<int16_t> pcm(pcm_samples);
  std::vector<uint8_t> encoded(G711_SAMPLES_PER_PACKET);
  // The 8 kHz signal, kept so it can be metered and looped back after the gain
  // stage -- that is, exactly what the far end will hear.
  std::vector<int16_t> narrowband(G711_SAMPLES_PER_PACKET);

  while (!this->should_stop_) {
    const size_t got = this->read_pcm_(pcm.data(), pcm_samples);
    if (got == 0)
      continue;

    this->mic_samples_ += static_cast<uint32_t>(got);
    this->mic_last_sample_us_ = esp_timer_get_time();

    const bool muted = this->config_.half_duplex && this->is_talking();

    size_t out_count = 0;
    for (size_t i = 0; i + decimation <= got; i += decimation) {
      int32_t acc = 0;
      for (uint32_t d = 0; d < decimation; d++)
        acc += pcm[i + d];
      // Averaging the pair is a cheap half-band filter; good enough to keep
      // 16 kHz content from aliasing into the 8 kHz G.711 band.
      acc /= static_cast<int32_t>(decimation);

      narrowband[out_count] = clamp_pcm16(static_cast<int32_t>(acc * this->config_.mic_gain));
      encoded[out_count] = this->compand_(muted ? 0 : narrowband[out_count]);
      out_count++;
      if (out_count == G711_SAMPLES_PER_PACKET)
        break;
    }

    if (out_count == 0)
      continue;

    // Metered before the half-duplex mute, on purpose: the meter answers "does
    // the microphone hear anything", and that question stays valid -- and worth
    // asking -- while the far end is talking and the mic is being held silent.
    this->update_hold_(&this->mic_hold_, &this->mic_hold_since_us_,
                       update_peak_(&this->mic_peak_, &this->mic_peak_us_, narrowband.data(), out_count));

    if (this->loopback_) {
      // Back up to the sink's rate before writing, or the monitor plays an
      // octave low and twice as slow. Sample-and-hold is enough here: the
      // signal really is band-limited to 4 kHz at this point, and holding each
      // sample reproduces what the far end hears rather than flattering it.
      size_t n = 0;
      for (size_t i = 0; i < out_count; i++)
        for (uint32_t d = 0; d < decimation; d++)
          pcm[n++] = narrowband[i];
      this->write_pcm_(pcm.data(), n);
    }

    if (this->callback_) {
      this->callback_(encoded.data(), out_count, this->rtp_timestamp_);
      this->rtp_timestamp_ += static_cast<uint32_t>(out_count);
      this->packets_sent_++;
    }
  }

  this->capture_task_ = nullptr;
  vTaskDelete(nullptr);
}

void AudioPipeline::playback_run_() {
  const uint32_t interpolation = this->config_.sample_rate / G711_SAMPLE_RATE;  // 1 or 2

  std::vector<uint8_t> encoded(G711_SAMPLES_PER_PACKET);
  std::vector<int16_t> pcm(G711_SAMPLES_PER_PACKET * interpolation);

  int16_t previous = 0;

  while (!this->should_stop_) {
    // The test beep owns the sink while it lasts: it is a deliberate bench
    // action, and mixing it with whatever else is playing would only make the
    // result harder to read.
    if (this->tone_remaining_ > 0) {
      const size_t count = this->take_test_tone_(pcm.data(), pcm.size());
      if (count > 0) {
        this->write_pcm_(pcm.data(), count);
        // Synthesis is far faster than playback, so pace it. Without this the
        // loop hands the sink a second of tone in a few milliseconds and an
        // ESPHome speaker, which drops rather than blocks, plays a click.
        vTaskDelay(pdMS_TO_TICKS((count * 1000) / this->config_.sample_rate));
        continue;
      }
    }

    const size_t got = xStreamBufferReceive(this->playback_buffer_, encoded.data(), encoded.size(),
                                            pdMS_TO_TICKS(20));
    if (got == 0) {
      // Nothing queued mid-sentence: keep the sink fed with silence so the DMA
      // never underruns and the amplifier does not click.
      if (this->is_talking() && !this->loopback_) {
        std::fill(pcm.begin(), pcm.end(), static_cast<int16_t>(0));
        this->write_pcm_(pcm.data(), pcm.size());
      } else {
        previous = 0;
      }
      continue;
    }

    // While the loopback monitor is on, the capture task owns the speaker.
    // Two tasks pushing into one sink interleave into noise, and the backchannel
    // is not what is being tested at that moment anyway.
    if (this->loopback_) {
      previous = 0;
      continue;
    }

    size_t out_samples = 0;
    for (size_t i = 0; i < got; i++) {
      const int16_t sample =
          clamp_pcm16(static_cast<int32_t>(this->expand_(encoded[i]) * this->config_.speaker_volume));

      for (uint32_t k = 0; k < interpolation; k++) {
        // Linear interpolation between the previous and current 8 kHz sample.
        const int32_t interpolated =
            previous + (static_cast<int32_t>(sample - previous) * static_cast<int32_t>(k + 1) /
                        static_cast<int32_t>(interpolation));
        pcm[out_samples++] = clamp_pcm16(interpolated);
      }
      previous = sample;
    }

    this->write_pcm_(pcm.data(), out_samples);
  }

  this->playback_task_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
