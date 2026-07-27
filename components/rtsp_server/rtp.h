#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace rtsp_server {

/// Size of the fixed RTP header (RFC 3550, no CSRC / no extension).
static constexpr size_t RTP_HEADER_SIZE = 12;
/// Size of the RTSP "interleaved" framing header: '$' + channel + 16-bit length.
static constexpr size_t INTERLEAVED_HEADER_SIZE = 4;

/// Logical streams advertised in the SDP. The order matters: it is the order the
/// media descriptions appear in, which is what clients use to derive track IDs.
enum class StreamKind : uint8_t {
  VIDEO = 0,        ///< MJPEG, server -> client
  AUDIO = 1,        ///< G.711, server -> client (microphone)
  BACKCHANNEL = 2,  ///< G.711, client -> server (speaker, ONVIF backchannel)
};

/// Sink for fully-formed RTP packets.
///
/// Buffer convention: `buf[0..3]` is scratch space reserved for the RTSP
/// interleaved framing header, and the RTP packet itself starts at `buf[4]` and
/// is `rtp_len` bytes long. This lets a packetizer hand off a packet that the
/// transport can frame and push with a single `send()` and no copy.
class RtpSender {
 public:
  virtual ~RtpSender() = default;
  virtual void send_rtp(StreamKind kind, uint8_t *buf, size_t rtp_len) = 0;
};

/// Write a 12-byte RTP header at `dst`.
void write_rtp_header(uint8_t *dst, uint8_t payload_type, bool marker, uint16_t seq, uint32_t timestamp,
                      uint32_t ssrc);

/// Everything RFC 2435 needs from a JFIF frame.
struct JpegInfo {
  const uint8_t *scan{nullptr};  ///< entropy-coded data, EOI excluded
  size_t scan_len{0};
  const uint8_t *luma_table{nullptr};    ///< 64 bytes, zig-zag order
  const uint8_t *chroma_table{nullptr};  ///< 64 bytes, zig-zag order
  uint16_t width{0};
  uint16_t height{0};
  uint16_t restart_interval{0};
  uint8_t type{1};  ///< 0 = 4:2:2, 1 = 4:2:0 (RFC 2435 §4.1)
  bool valid{false};
};

/// Parse the JFIF markers a RFC 2435 payload header is built from.
JpegInfo parse_jpeg(const uint8_t *data, size_t len);

/// Packetizes JPEG frames per RFC 2435.
///
/// The JFIF headers are not transmitted: they are rebuilt by the receiver from
/// the 8-byte payload header, plus the quantization tables that are sent inline
/// in the first packet of every frame (Q = 255).
class MjpegPacketizer {
 public:
  MjpegPacketizer(size_t max_packet_size, uint8_t payload_type, uint32_t ssrc);

  /// Returns false when the frame is not a JPEG this packetizer can describe.
  bool packetize(const uint8_t *jpeg, size_t len, uint32_t timestamp, RtpSender *sink);

  uint32_t ssrc() const { return this->ssrc_; }

 private:
  std::vector<uint8_t> buf_;
  size_t max_packet_size_;
  uint8_t payload_type_;
  uint32_t ssrc_;
  uint16_t seq_{0};
};

/// Packetizes G.711 (PCMU/PCMA) frames, one RTP packet per audio chunk.
class G711Packetizer {
 public:
  G711Packetizer(size_t max_samples, uint8_t payload_type, uint32_t ssrc);

  /// `samples` holds `count` already-companded G.711 bytes.
  void packetize(const uint8_t *samples, size_t count, uint32_t timestamp, RtpSender *sink);

  uint32_t ssrc() const { return this->ssrc_; }

 private:
  std::vector<uint8_t> buf_;
  uint8_t payload_type_;
  uint32_t ssrc_;
  uint16_t seq_{0};
};

/// Base64 encoder used for the RTSP `Authorization: Basic` credentials.
std::string base64_encode(const uint8_t *data, size_t len);

}  // namespace rtsp_server
}  // namespace esphome
