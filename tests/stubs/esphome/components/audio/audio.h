#pragma once
#include <cstdint>
namespace esphome { namespace audio {
class AudioStreamInfo { public: AudioStreamInfo(uint8_t bits, uint8_t channels, uint32_t rate); };
} }
