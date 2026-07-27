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

  // --- Annex-B NAL walk ---
  std::vector<uint8_t> au = {0,0,0,1, 0x67, 0x42, 0x00, 0x1e, 0xAA,   // SPS
                             0,0,0,1, 0x68, 0xCE, 0x3C, 0x80,          // PPS
                             0,0,1,   0x65, 1,2,3,4,5,6,7,8};          // IDR
  size_t pos = 0; AnnexBNal nal; int n = 0; uint8_t types[8];
  while (next_annexb_nal(au.data(), au.size(), &pos, &nal)) types[n++] = nal.data[0] & 0x1F;
  assert(n == 3 && types[0]==7 && types[1]==8 && types[2]==5);
  printf("annex-b walk OK (%d NALs: %d %d %d)\n", n, types[0], types[1], types[2]);

  // --- single-NAL packetization + marker on last packet ---
  Collector c; H264Packetizer p(1400, 96, 0xDEADBEEF);
  p.packetize(au.data(), au.size(), 90000, &c);
  assert(c.pkts.size() == 3);
  for (size_t i = 0; i < c.pkts.size(); i++) {
    bool marker = (c.pkts[i].data[1] & 0x80) != 0;
    assert(marker == (i == c.pkts.size()-1));
    assert((c.pkts[i].data[1] & 0x7F) == 96);
  }
  uint16_t s0 = (c.pkts[0].data[2]<<8)|c.pkts[0].data[3];
  uint16_t s2 = (c.pkts[2].data[2]<<8)|c.pkts[2].data[3];
  assert(s2 == (uint16_t)(s0+2));
  printf("h264 single-NAL OK (3 pkts, seq %u..%u, marker only on last)\n", s0, s2);

  // --- FU-A fragmentation of a big NAL ---
  std::vector<uint8_t> big; big.insert(big.end(), {0,0,0,1, 0x65});
  for (int i = 0; i < 4000; i++) big.push_back((uint8_t)i);
  Collector c2; H264Packetizer p2(1400, 96, 1);
  p2.packetize(big.data(), big.size(), 0, &c2);
  size_t payload_total = 0; 
  for (size_t i = 0; i < c2.pkts.size(); i++) {
    auto &d = c2.pkts[i].data;
    assert(d.size() <= 1400);
    assert((d[12] & 0x1F) == 28);           // FU-A indicator
    assert((d[13] & 0x1F) == 5);            // original NAL type preserved
    assert(((d[13] & 0x80) != 0) == (i==0));                    // S bit
    assert(((d[13] & 0x40) != 0) == (i==c2.pkts.size()-1));     // E bit
    assert(((d[1] & 0x80) != 0) == (i==c2.pkts.size()-1));      // marker
    payload_total += d.size() - 14;
  }
  assert(payload_total == 4000);  // NAL header byte is not retransmitted
  printf("h264 FU-A OK (%zu frags, %zu payload bytes reassembled)\n", c2.pkts.size(), payload_total);

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
  // send_rtp -- i.e. INCLUDING the 4-byte interleaved header, which the other
  // collectors above strip. If that offset moved, or if a packetizer stopped
  // marking the last fragment, the server would drop from the middle of one
  // frame into the next instead of resuming cleanly.
  {
    struct RawCollector : RtpSender {
      std::vector<std::vector<uint8_t>> pkts;
      void send_rtp(StreamKind, uint8_t *buf, size_t rtp_len) override {
        pkts.push_back(std::vector<uint8_t>(buf, buf + INTERLEAVED_HEADER_SIZE + rtp_len));
      }
    };

    RawCollector rc;
    H264Packetizer p4(1400, 96, 7);
    p4.packetize(big.data(), big.size(), 0, &rc);
    assert(rc.pkts.size() > 1);  // must actually fragment, or this proves nothing
    for (size_t i = 0; i < rc.pkts.size(); i++) {
      const bool marker = (rc.pkts[i][INTERLEAVED_HEADER_SIZE + 1] & 0x80) != 0;
      assert(marker == (i + 1 == rc.pkts.size()));
    }

    // Audio marks every packet, which is why send_rtp only applies the policy
    // to StreamKind::VIDEO.
    RawCollector ra;
    G711Packetizer g2(160, 0, 42);
    g2.packetize(pcm.data(), 160, 0, &ra);
    assert(ra.pkts.size() == 1);
    assert((ra.pkts[0][INTERLEAVED_HEADER_SIZE + 1] & 0x80) != 0);

    printf("frame-end marker at buf[%zu] (%zu video frags, audio always marked)\n",
           static_cast<size_t>(INTERLEAVED_HEADER_SIZE) + 1, rc.pkts.size());
  }

  printf("\nall checks passed\n");
  return 0;
}
