#pragma once

#include <cstdint>

namespace esphome {
namespace rtsp_server {

/// ITU-T G.711 µ-law / A-law conversion.
///
/// Reference implementation derived from the CCITT/Sun public-domain g711.c.
/// Both codecs map a 16-bit linear PCM sample onto a single byte, which is what
/// RTP payload types 0 (PCMU) and 8 (PCMA) carry.

namespace g711 {

static inline int seg_search_(int val, const int16_t *table, int size) {
  for (int i = 0; i < size; i++) {
    if (val <= table[i])
      return i;
  }
  return size;
}

/// 16-bit linear PCM -> µ-law (RTP payload type 0).
static inline uint8_t linear_to_ulaw(int16_t pcm) {
  static const int16_t SEG_UEND[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};

  int mask;
  int value = pcm >> 2;  // µ-law works on a 14-bit magnitude

  if (value < 0) {
    value = -value;
    mask = 0x7F;
  } else {
    mask = 0xFF;
  }
  if (value > 0x1FFF)
    value = 0x1FFF;
  value += (0x84 >> 2);

  const int seg = seg_search_(value, SEG_UEND, 8);
  if (seg >= 8)
    return static_cast<uint8_t>(0x7F ^ mask);

  const uint8_t uval = static_cast<uint8_t>((seg << 4) | ((value >> (seg + 1)) & 0x0F));
  return static_cast<uint8_t>(uval ^ mask);
}

/// µ-law -> 16-bit linear PCM.
static inline int16_t ulaw_to_linear(uint8_t uval) {
  const int u = ~uval;
  int t = ((u & 0x0F) << 3) + 0x84;
  t <<= (static_cast<unsigned>(u) & 0x70) >> 4;
  return static_cast<int16_t>((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

/// 16-bit linear PCM -> A-law (RTP payload type 8).
static inline uint8_t linear_to_alaw(int16_t pcm) {
  static const int16_t SEG_AEND[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};

  int mask;
  int value = pcm >> 3;  // A-law works on a 13-bit magnitude

  if (value >= 0) {
    mask = 0xD5;  // sign (7th) bit = 1
  } else {
    mask = 0x55;  // sign bit = 0
    value = -value - 1;
  }
  if (value > 0xFFF)
    value = 0xFFF;

  const int seg = seg_search_(value, SEG_AEND, 8);
  if (seg >= 8)
    return static_cast<uint8_t>(0x7F ^ mask);

  uint8_t aval = static_cast<uint8_t>(seg << 4);
  if (seg < 2)
    aval |= static_cast<uint8_t>((value >> 1) & 0x0F);
  else
    aval |= static_cast<uint8_t>((value >> seg) & 0x0F);
  return static_cast<uint8_t>(aval ^ mask);
}

/// A-law -> 16-bit linear PCM.
static inline int16_t alaw_to_linear(uint8_t aval) {
  const int a = aval ^ 0x55;
  int t = (a & 0x0F) << 4;
  const int seg = (static_cast<unsigned>(a) & 0x70) >> 4;
  switch (seg) {
    case 0:
      t += 8;
      break;
    case 1:
      t += 0x108;
      break;
    default:
      t += 0x108;
      t <<= seg - 1;
      break;
  }
  return static_cast<int16_t>((a & 0x80) ? t : -t);
}

}  // namespace g711
}  // namespace rtsp_server
}  // namespace esphome
