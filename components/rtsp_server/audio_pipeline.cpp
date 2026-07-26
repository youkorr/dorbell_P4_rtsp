#include "audio_pipeline.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cinttypes>
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

void AudioPipeline::write_pcm_(const int16_t *src, size_t samples) {
  if (samples == 0)
    return;

  if (this->external_speaker_ != nullptr) {
    this->external_speaker_->play(reinterpret_cast<const uint8_t *>(src), samples * sizeof(int16_t));
    return;
  }
  if (this->tx_handle_ == nullptr)
    return;

  const size_t bytes_per_sample = this->config_.speaker_bits / 8;
  if (bytes_per_sample == 2) {
    size_t written = 0;
    i2s_channel_write(this->tx_handle_, src, samples * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
    return;
  }

  std::vector<int32_t> wide(samples);
  for (size_t i = 0; i < samples; i++)
    wide[i] = static_cast<int32_t>(src[i]) << 16;
  size_t written = 0;
  i2s_channel_write(this->tx_handle_, wide.data(), wide.size() * sizeof(int32_t), &written, pdMS_TO_TICKS(200));
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

  while (!this->should_stop_) {
    const size_t got = this->read_pcm_(pcm.data(), pcm_samples);
    if (got == 0)
      continue;

    const bool muted = this->config_.half_duplex && this->is_talking();

    size_t out_count = 0;
    for (size_t i = 0; i + decimation <= got; i += decimation) {
      int32_t acc = 0;
      for (uint32_t d = 0; d < decimation; d++)
        acc += pcm[i + d];
      // Averaging the pair is a cheap half-band filter; good enough to keep
      // 16 kHz content from aliasing into the 8 kHz G.711 band.
      acc /= static_cast<int32_t>(decimation);

      if (muted) {
        acc = 0;
      } else {
        acc = static_cast<int32_t>(acc * this->config_.mic_gain);
        acc = std::max(-32768, std::min(32767, acc));
      }

      encoded[out_count++] = this->compand_(static_cast<int16_t>(acc));
      if (out_count == G711_SAMPLES_PER_PACKET)
        break;
    }

    if (out_count > 0 && this->callback_) {
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
    const size_t got = xStreamBufferReceive(this->playback_buffer_, encoded.data(), encoded.size(),
                                            pdMS_TO_TICKS(20));
    if (got == 0) {
      // Nothing queued mid-sentence: keep the sink fed with silence so the DMA
      // never underruns and the amplifier does not click.
      if (this->is_talking()) {
        std::fill(pcm.begin(), pcm.end(), static_cast<int16_t>(0));
        this->write_pcm_(pcm.data(), pcm.size());
      } else {
        previous = 0;
      }
      continue;
    }

    size_t out_samples = 0;
    for (size_t i = 0; i < got; i++) {
      int32_t sample = this->expand_(encoded[i]);
      sample = static_cast<int32_t>(sample * this->config_.speaker_volume);
      sample = std::max(-32768, std::min(32767, sample));

      for (uint32_t k = 0; k < interpolation; k++) {
        // Linear interpolation between the previous and current 8 kHz sample.
        const int32_t interpolated =
            previous + static_cast<int32_t>((sample - previous) * static_cast<int32_t>(k + 1) /
                                            static_cast<int32_t>(interpolation));
        pcm[out_samples++] = static_cast<int16_t>(interpolated);
      }
      previous = static_cast<int16_t>(sample);
    }

    this->write_pcm_(pcm.data(), out_samples);
  }

  this->playback_task_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
