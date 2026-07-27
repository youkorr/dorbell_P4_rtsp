#pragma once
#include <cstdint>
#include <cstddef>
#include "esphome/components/audio/audio.h"
namespace esphome { namespace speaker {
class Speaker {
 public:
  virtual size_t play(const uint8_t *data, size_t length) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  void set_audio_stream_info(const audio::AudioStreamInfo &info) {}
};
} }
