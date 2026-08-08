#pragma once
#include <cstddef>
#include <cstdint>
#include "esphome/components/audio/audio.h"
#include "freertos/FreeRTOS.h"

// Stub of esphome/components/speaker/speaker.h. Signatures copied from the real
// header: `play` RETURNS the number of bytes the speaker accepted, and ignoring
// that return is how audio disappears without leaving a trace anywhere.
namespace esphome {
namespace speaker {

class Speaker {
 public:
  /// Blocking variant: waits up to `ticks_to_wait` for room in the ring buffer.
  virtual size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
    return this->play(data, length);
  }
  /// @return bytes actually written -- may be less than `length`, and is 0 when
  /// the speaker is not running.
  virtual size_t play(const uint8_t *data, size_t length) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  bool is_running() const { return true; }
  bool is_stopped() const { return false; }
  virtual void set_volume(float volume) {}
  virtual void set_mute_state(bool mute_state) {}
  void set_audio_stream_info(const audio::AudioStreamInfo &info) {}
};

}  // namespace speaker
}  // namespace esphome
