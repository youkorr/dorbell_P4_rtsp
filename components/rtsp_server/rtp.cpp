#include "rtp.h"

#include <cstring>

namespace esphome {
namespace rtsp_server {

void write_rtp_header(uint8_t *dst, uint8_t payload_type, bool marker, uint16_t seq, uint32_t timestamp,
                      uint32_t ssrc) {
  dst[0] = 0x80;  // version 2, no padding, no extension, 0 CSRC
  dst[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (payload_type & 0x7F));
  dst[2] = static_cast<uint8_t>(seq >> 8);
  dst[3] = static_cast<uint8_t>(seq);
  dst[4] = static_cast<uint8_t>(timestamp >> 24);
  dst[5] = static_cast<uint8_t>(timestamp >> 16);
  dst[6] = static_cast<uint8_t>(timestamp >> 8);
  dst[7] = static_cast<uint8_t>(timestamp);
  dst[8] = static_cast<uint8_t>(ssrc >> 24);
  dst[9] = static_cast<uint8_t>(ssrc >> 16);
  dst[10] = static_cast<uint8_t>(ssrc >> 8);
  dst[11] = static_cast<uint8_t>(ssrc);
}

/// Returns the length of the start code at `buf + i`, or 0 if there is none.
static inline size_t start_code_len(const uint8_t *buf, size_t len, size_t i) {
  if (i + 3 <= len && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
    return 3;
  if (i + 4 <= len && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1)
    return 4;
  return 0;
}

bool next_annexb_nal(const uint8_t *buf, size_t len, size_t *pos, AnnexBNal *out) {
  size_t i = *pos;

  // Skip to the next start code.
  size_t sc = 0;
  while (i < len && (sc = start_code_len(buf, len, i)) == 0)
    i++;
  if (i >= len)
    return false;

  const size_t nal_start = i + sc;
  if (nal_start >= len)
    return false;

  // Find the following start code (or the end of the buffer).
  size_t j = nal_start;
  while (j < len && start_code_len(buf, len, j) == 0)
    j++;

  out->data = buf + nal_start;
  out->size = j - nal_start;
  *pos = j;
  return out->size > 0;
}

// ---------------------------------------------------------------------------
// H.264
// ---------------------------------------------------------------------------

H264Packetizer::H264Packetizer(size_t max_packet_size, uint8_t payload_type, uint32_t ssrc)
    : max_packet_size_(max_packet_size), payload_type_(payload_type), ssrc_(ssrc) {
  this->buf_.resize(INTERLEAVED_HEADER_SIZE + max_packet_size);
}

void H264Packetizer::packetize(const uint8_t *au, size_t au_len, uint32_t timestamp, RtpSender *sink) {
  size_t pos = 0;
  AnnexBNal nal{};
  AnnexBNal pending{};
  bool have_pending = false;

  // One NAL of look-ahead so the marker bit lands on the last packet of the
  // access unit rather than the last packet of every NAL.
  while (next_annexb_nal(au, au_len, &pos, &nal)) {
    if (have_pending)
      this->emit_nal_(pending, timestamp, false, sink);
    pending = nal;
    have_pending = true;
  }
  if (have_pending)
    this->emit_nal_(pending, timestamp, true, sink);
}

void H264Packetizer::emit_nal_(const AnnexBNal &nal, uint32_t timestamp, bool last_in_au, RtpSender *sink) {
  uint8_t *buf = this->buf_.data();
  uint8_t *rtp = buf + INTERLEAVED_HEADER_SIZE;

  if (RTP_HEADER_SIZE + nal.size <= this->max_packet_size_) {
    // Single NAL unit packet.
    write_rtp_header(rtp, this->payload_type_, last_in_au, this->seq_++, timestamp, this->ssrc_);
    std::memcpy(rtp + RTP_HEADER_SIZE, nal.data, nal.size);
    sink->send_rtp(StreamKind::VIDEO, buf, RTP_HEADER_SIZE + nal.size);
    return;
  }

  // FU-A fragmentation (RFC 6184 §5.8).
  const uint8_t nri = static_cast<uint8_t>(nal.data[0] & 0x60);
  const uint8_t type = static_cast<uint8_t>(nal.data[0] & 0x1F);
  const uint8_t fu_indicator = static_cast<uint8_t>(nri | 28);

  const size_t max_payload = this->max_packet_size_ - RTP_HEADER_SIZE - 2;
  const uint8_t *src = nal.data + 1;  // the original NAL header is not transmitted
  size_t remaining = nal.size - 1;
  bool first = true;

  while (remaining > 0) {
    const size_t chunk = remaining < max_payload ? remaining : max_payload;
    const bool last = (chunk == remaining);

    uint8_t fu_header = type;
    if (first)
      fu_header |= 0x80;  // S bit
    if (last)
      fu_header |= 0x40;  // E bit

    write_rtp_header(rtp, this->payload_type_, last && last_in_au, this->seq_++, timestamp, this->ssrc_);
    rtp[RTP_HEADER_SIZE] = fu_indicator;
    rtp[RTP_HEADER_SIZE + 1] = fu_header;
    std::memcpy(rtp + RTP_HEADER_SIZE + 2, src, chunk);
    sink->send_rtp(StreamKind::VIDEO, buf, RTP_HEADER_SIZE + 2 + chunk);

    src += chunk;
    remaining -= chunk;
    first = false;
  }
}

// ---------------------------------------------------------------------------
// MJPEG (RFC 2435)
// ---------------------------------------------------------------------------

static inline uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

JpegInfo parse_jpeg(const uint8_t *data, size_t len) {
  JpegInfo info;
  const uint8_t *tables[4] = {nullptr, nullptr, nullptr, nullptr};

  if (len < 4 || data[0] != 0xFF || data[1] != 0xD8)  // SOI
    return info;

  size_t pos = 2;
  while (pos + 4 <= len) {
    if (data[pos] != 0xFF)
      return info;

    const uint8_t marker = data[pos + 1];
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      pos += 2;  // standalone markers carry no length
      continue;
    }
    if (marker == 0xD9)  // EOI before any scan
      return info;

    const size_t seg_len = be16(data + pos + 2);
    if (seg_len < 2 || pos + 2 + seg_len > len)
      return info;
    const uint8_t *seg = data + pos + 4;
    const size_t seg_data_len = seg_len - 2;

    switch (marker) {
      case 0xDB: {  // DQT, possibly several tables in one segment
        size_t off = 0;
        while (off + 65 <= seg_data_len) {
          const uint8_t precision = static_cast<uint8_t>(seg[off] >> 4);
          const uint8_t table_id = static_cast<uint8_t>(seg[off] & 0x0F);
          if (precision != 0 || table_id > 3)
            return info;  // RFC 2435 only carries 8-bit tables
          tables[table_id] = seg + off + 1;
          off += 65;
        }
        break;
      }
      case 0xC0:    // SOF0, baseline
      case 0xC1: {  // SOF1, extended sequential (same layout)
        if (seg_data_len < 6)
          return info;
        info.height = be16(seg + 1);
        info.width = be16(seg + 3);
        const uint8_t components = seg[5];
        if (components < 1 || seg_data_len < static_cast<size_t>(6 + components * 3))
          return info;
        // Sampling factors of the luma component decide the RFC 2435 type.
        const uint8_t sampling = seg[7];
        const uint8_t h = static_cast<uint8_t>(sampling >> 4);
        const uint8_t v = static_cast<uint8_t>(sampling & 0x0F);
        if (h == 2 && v == 2) {
          info.type = 1;  // 4:2:0
        } else if (h == 2 && v == 1) {
          info.type = 0;  // 4:2:2
        } else {
          return info;  // 4:4:4 and friends have no RFC 2435 type
        }
        break;
      }
      case 0xDD:  // DRI
        if (seg_data_len >= 2)
          info.restart_interval = be16(seg);
        break;
      case 0xDA: {  // SOS: the entropy-coded data starts right after it
        const size_t scan_start = pos + 2 + seg_len;
        if (scan_start >= len)
          return info;
        // RFC 2435 carries the entropy-coded scan alone, so everything from the
        // EOI marker onwards has to go. Trimming only the last two bytes is not
        // enough: the ESP32-P4 hardware JPEG encoder reports a length rounded up
        // to its DMA burst, so alignment bytes usually sit AFTER the EOI. Those
        // bytes then travel as scan data and the receiver decodes the embedded
        // FF D9 as a premature end of image — which strict decoders report as
        // "error dc" partway through the picture while lenient ones (VLC, most
        // browsers) just show a torn frame.
        //
        // Scan forward for the FIRST EOI rather than backwards from the end: a
        // padding byte pair could look like an EOI too. Inside entropy-coded
        // data an 0xFF byte is always followed by 0x00 (byte stuffing) or by a
        // restart marker 0xD0..0xD7, so FF D9 cannot occur before the real EOI.
        size_t scan_end = len;
        for (const uint8_t *p = data + scan_start; p + 1 < data + len;) {
          const auto *ff = static_cast<const uint8_t *>(std::memchr(p, 0xFF, static_cast<size_t>(data + len - 1 - p)));
          if (ff == nullptr)
            break;
          if (ff[1] == 0xD9) {
            scan_end = static_cast<size_t>(ff - data);
            break;
          }
          p = ff + 1;
        }
        if (scan_end <= scan_start)
          return info;

        info.scan = data + scan_start;
        info.scan_len = scan_end - scan_start;
        info.luma_table = tables[0];
        info.chroma_table = tables[1] != nullptr ? tables[1] : tables[0];
        info.valid = info.width != 0 && info.height != 0 && info.width <= 2040 && info.height <= 2040 &&
                     info.luma_table != nullptr && info.chroma_table != nullptr;
        return info;
      }
      default:
        break;
    }

    pos += 2 + seg_len;
  }

  return info;
}

MjpegPacketizer::MjpegPacketizer(size_t max_packet_size, uint8_t payload_type, uint32_t ssrc)
    : max_packet_size_(max_packet_size), payload_type_(payload_type), ssrc_(ssrc) {
  this->buf_.resize(INTERLEAVED_HEADER_SIZE + max_packet_size);
}

bool MjpegPacketizer::packetize(const uint8_t *jpeg, size_t len, uint32_t timestamp, RtpSender *sink) {
  const JpegInfo info = parse_jpeg(jpeg, len);
  if (!info.valid)
    return false;

  // RFC 2435 §3.1: 8-byte main header, plus a 4-byte quantization table header
  // and 128 bytes of tables on the first fragment only.
  static constexpr size_t MAIN_HEADER = 8;
  static constexpr size_t RESTART_HEADER = 4;
  static constexpr size_t TABLE_BYTES = 128;
  static constexpr size_t TABLE_HEADER = 4 + TABLE_BYTES;

  const bool has_restarts = info.restart_interval != 0;
  const size_t extra = has_restarts ? RESTART_HEADER : 0;
  const uint8_t type = static_cast<uint8_t>(info.type + (has_restarts ? 64 : 0));

  uint8_t *buf = this->buf_.data();
  uint8_t *rtp = buf + INTERLEAVED_HEADER_SIZE;

  size_t offset = 0;
  while (offset < info.scan_len) {
    const bool first = offset == 0;
    const size_t header_size = MAIN_HEADER + extra + (first ? TABLE_HEADER : 0);
    if (RTP_HEADER_SIZE + header_size >= this->max_packet_size_)
      return false;  // packet_size too small to carry the tables

    size_t payload = this->max_packet_size_ - RTP_HEADER_SIZE - header_size;
    const size_t remaining = info.scan_len - offset;
    if (payload > remaining)
      payload = remaining;
    const bool last = (offset + payload) == info.scan_len;

    write_rtp_header(rtp, this->payload_type_, last, this->seq_++, timestamp, this->ssrc_);

    uint8_t *p = rtp + RTP_HEADER_SIZE;
    p[0] = 0;  // type-specific
    p[1] = static_cast<uint8_t>(offset >> 16);
    p[2] = static_cast<uint8_t>(offset >> 8);
    p[3] = static_cast<uint8_t>(offset);
    p[4] = type;
    p[5] = 255;  // Q = 255: the quantization tables travel in-band
    p[6] = static_cast<uint8_t>(info.width / 8);
    p[7] = static_cast<uint8_t>(info.height / 8);
    p += MAIN_HEADER;

    if (has_restarts) {
      p[0] = static_cast<uint8_t>(info.restart_interval >> 8);
      p[1] = static_cast<uint8_t>(info.restart_interval);
      p[2] = 0xFF;  // F = L = 1, count = 0x3FFF (restart markers are not counted)
      p[3] = 0xFF;
      p += RESTART_HEADER;
    }

    if (first) {
      p[0] = 0;  // MBZ
      p[1] = 0;  // precision: both tables are 8-bit
      p[2] = static_cast<uint8_t>(TABLE_BYTES >> 8);
      p[3] = static_cast<uint8_t>(TABLE_BYTES);
      std::memcpy(p + 4, info.luma_table, 64);
      std::memcpy(p + 4 + 64, info.chroma_table, 64);
      p += TABLE_HEADER;
    }

    std::memcpy(p, info.scan + offset, payload);
    sink->send_rtp(StreamKind::VIDEO, buf, RTP_HEADER_SIZE + header_size + payload);

    offset += payload;
  }

  return true;
}

// ---------------------------------------------------------------------------
// G.711
// ---------------------------------------------------------------------------

G711Packetizer::G711Packetizer(size_t max_samples, uint8_t payload_type, uint32_t ssrc)
    : payload_type_(payload_type), ssrc_(ssrc) {
  this->buf_.resize(INTERLEAVED_HEADER_SIZE + RTP_HEADER_SIZE + max_samples);
}

void G711Packetizer::packetize(const uint8_t *samples, size_t count, uint32_t timestamp, RtpSender *sink) {
  const size_t capacity = this->buf_.size() - INTERLEAVED_HEADER_SIZE - RTP_HEADER_SIZE;
  if (count > capacity)
    count = capacity;

  uint8_t *buf = this->buf_.data();
  uint8_t *rtp = buf + INTERLEAVED_HEADER_SIZE;

  // Marker is set on every audio packet: harmless for RTP, and it is what
  // go2rtc does on its own audio senders.
  write_rtp_header(rtp, this->payload_type_, true, this->seq_++, timestamp, this->ssrc_);
  std::memcpy(rtp + RTP_HEADER_SIZE, samples, count);
  sink->send_rtp(StreamKind::AUDIO, buf, RTP_HEADER_SIZE + count);
}

// ---------------------------------------------------------------------------

std::string base64_encode(const uint8_t *data, size_t len) {
  static const char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  size_t i = 0;
  while (i + 3 <= len) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out += TABLE[(v >> 18) & 0x3F];
    out += TABLE[(v >> 12) & 0x3F];
    out += TABLE[(v >> 6) & 0x3F];
    out += TABLE[v & 0x3F];
    i += 3;
  }

  if (i + 1 == len) {
    const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    out += TABLE[(v >> 18) & 0x3F];
    out += TABLE[(v >> 12) & 0x3F];
    out += "==";
  } else if (i + 2 == len) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out += TABLE[(v >> 18) & 0x3F];
    out += TABLE[(v >> 12) & 0x3F];
    out += TABLE[(v >> 6) & 0x3F];
    out += '=';
  }

  return out;
}

}  // namespace rtsp_server
}  // namespace esphome
