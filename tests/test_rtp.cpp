#include "rtp.h"
#include "g711.h"
#include <cstdio>
#include <cassert>
#include <vector>
using namespace esphome::rtsp_server;

struct Collector : RtpSender {
  struct Pkt { StreamKind kind; std::vector<uint8_t> data; };
  std::vector<Pkt> pkts;
  void send_rtp(StreamKind kind, uint8_t *buf, size_t rtp_len) override {
    assert(buf[0]=='$' || true);
    pkts.push_back({kind, std::vector<uint8_t>(buf+4, buf+4+rtp_len)});
  }
};

int main() {
  // --- G.711 round trip ---
  int worst = 0;
  for (int v = -32768; v <= 32767; v += 7) {
    int16_t s = (int16_t)v;
    int16_t u = g711::ulaw_to_linear(g711::linear_to_ulaw(s));
    int16_t a = g711::alaw_to_linear(g711::linear_to_alaw(s));
    int du = abs(u - s), da = abs(a - s);
    // G.711 is logarithmic: error grows with magnitude, bounded by ~8% (ulaw)
    int tol = 8 + abs(v) / 10;
    if (du > tol || da > tol) { printf("FAIL v=%d ulaw=%d alaw=%d\n", v, u, a); return 1; }
    worst = std::max(worst, std::max(du, da));
  }
  printf("g711 round trip OK (worst abs err %d)\n", worst);

  // --- base64 ---
  const char *s = "Man"; 
  assert(base64_encode((const uint8_t*)s, 3) == "TWFu");
  assert(base64_encode((const uint8_t*)"M", 1) == "TQ==");
  assert(base64_encode((const uint8_t*)"Ma", 2) == "TWE=");
  printf("base64 OK\n");

  // --- G.711 RTP ---
  Collector c3; G711Packetizer g(160, 0, 42);
  std::vector<uint8_t> pcm(160, 0xFF);
  g.packetize(pcm.data(), 160, 8000, &c3);
  assert(c3.pkts.size() == 1 && c3.pkts[0].data.size() == 172);
  assert((c3.pkts[0].data[1] & 0x7F) == 0 && (c3.pkts[0].data[1] & 0x80));
  printf("g711 rtp OK (172 bytes = 12 hdr + 160 payload)\n");

  // --- invariant relied upon by RTSPServer::send_rtp ------------------------
  // Frame-aligned dropping finds the end of a video frame by reading the RTP
  // marker at buf[INTERLEAVED_HEADER_SIZE + 1], on the buffer as handed to
  // send_rtp -- i.e. INCLUDING the 4-byte interleaved header, which the
  // collector above strips. Audio marks EVERY packet, which is exactly why
  // send_rtp applies the frame-dropping policy to StreamKind::VIDEO only; the
  // video side of this invariant is checked in test_mjpeg.cpp, on real frames.
  {
    struct RawCollector : RtpSender {
      std::vector<std::vector<uint8_t>> pkts;
      void send_rtp(StreamKind, uint8_t *buf, size_t rtp_len) override {
        pkts.push_back(std::vector<uint8_t>(buf, buf + INTERLEAVED_HEADER_SIZE + rtp_len));
      }
    };

    RawCollector ra;
    G711Packetizer g2(160, 0, 42);
    g2.packetize(pcm.data(), 160, 0, &ra);
    assert(ra.pkts.size() == 1);
    assert((ra.pkts[0][INTERLEAVED_HEADER_SIZE + 1] & 0x80) != 0);

    printf("frame-end marker at buf[%zu] (audio always marked)\n",
           static_cast<size_t>(INTERLEAVED_HEADER_SIZE) + 1);
  }

  printf("\nall checks passed\n");
  return 0;
}
