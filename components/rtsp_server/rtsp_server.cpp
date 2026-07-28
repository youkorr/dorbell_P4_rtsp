#include "rtsp_server.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_netif.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rtsp_server {

static const char *const TAG = "rtsp_server";
static const char *const SERVER_NAME = "ESPHome-RTSP/1.0 (ESP32-P4)";

/// Holds roughly one large key frame plus a margin, so a burst of packets can
/// be queued while the network task is busy writing an earlier one.
static constexpr size_t TX_RING_BYTES = 192 * 1024;
static constexpr int SELECT_TIMEOUT_MS = 5;
static constexpr uint32_t SESSION_TIMEOUT_MS = 120000;

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

static std::string to_lower(const std::string &value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

/// Fetch an RTSP header value, case-insensitively. Returns "" when absent.
static std::string header_value(const std::string &request, const std::string &name) {
  const std::string lowered = to_lower(request);
  const std::string needle = "\r\n" + to_lower(name) + ":";

  size_t pos = lowered.find(needle);
  if (pos == std::string::npos)
    return "";

  pos += needle.size();
  const size_t end = request.find("\r\n", pos);
  if (end == std::string::npos)
    return "";

  std::string value = request.substr(pos, end - pos);
  const size_t first = value.find_first_not_of(" \t");
  if (first == std::string::npos)
    return "";
  const size_t last = value.find_last_not_of(" \t\r");
  return value.substr(first, last - first + 1);
}

static int parse_cseq(const std::string &request) {
  const std::string value = header_value(request, "CSeq");
  return value.empty() ? 0 : atoi(value.c_str());
}

/// Extract "trackID=N" from a SETUP URL. Returns -1 when not present.
static int parse_track_id(const std::string &url) {
  const size_t pos = url.find("trackID=");
  if (pos == std::string::npos)
    return -1;
  return atoi(url.c_str() + pos + 8);
}

/// The address a client should dial.
///
/// ESPHome's own network helpers have been renamed across releases
/// (`network::get_use_address()` is gone in 2026.8), so ask lwIP's default
/// interface directly: that API is stable and is the same answer anyway.
static std::string default_local_address() {
  esp_netif_t *netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t info = {};
  if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return std::string(buf);
  }
  return "0.0.0.0";
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void RTSPServer::set_credentials(const std::string &user, const std::string &password) {
  if (user.empty() && password.empty()) {
    this->auth_token_.clear();
    return;
  }
  const std::string raw = user + ":" + password;
  this->auth_token_ = "Basic " + base64_encode(reinterpret_cast<const uint8_t *>(raw.data()), raw.size());
}

void RTSPServer::setup() {
  this->tx_ring_ = xRingbufferCreate(TX_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
  if (this->tx_ring_ == nullptr) {
    ESP_LOGE(TAG, "failed to allocate the %u KiB transmit ring buffer",
             static_cast<unsigned>(TX_RING_BYTES / 1024));
    this->mark_failed();
    return;
  }

  // RFC 2435 assigns JPEG the static payload type 26.
  this->mjpeg_packetizer_ = make_unique<MjpegPacketizer>(this->packet_size_, 26, random_uint32());

  const uint8_t audio_payload_type = static_cast<uint8_t>(this->audio_config_.codec);
  this->audio_packetizer_ = make_unique<G711Packetizer>(G711_SAMPLES_PER_PACKET, audio_payload_type, random_uint32());
}

void RTSPServer::loop() {
  // The V4L2 devices and the listening socket are only brought up once the
  // network is available, so a boot without Wi-Fi does not fail the component.
  if (!this->pipelines_started_) {
    if (!network::is_connected())
      return;
    if (!this->start_pipelines_()) {
      this->mark_failed();
      return;
    }
    this->pipelines_started_ = true;
  }

  // Triggers are fired here, on the main loop, never from a FreeRTOS task.
  const uint8_t clients = this->client_count_;
  if (clients != this->last_reported_clients_) {
    if (clients > this->last_reported_clients_) {
      this->client_connected_callback_.call();
    } else {
      this->client_disconnected_callback_.call();
    }
    this->last_reported_clients_ = clients;
  }

  const bool talking = this->audio_.is_talking();
  if (talking != this->last_reported_talking_) {
    if (talking) {
      this->talk_start_callback_.call();
    } else {
      this->talk_end_callback_.call();
    }
    this->last_reported_talking_ = talking;
  }

  // Periodic summary. Every 10 s while a client is connected -- the window in
  // which a fault is actually being reproduced -- and every 60 s when idle, so
  // an unattended log still shows the doorbell is alive without drowning it.
  const uint32_t now = millis();
  const uint32_t period = clients > 0 ? 10000 : 60000;
  if (now - this->last_status_ms_ >= period) {
    this->last_status_ms_ = now;
    this->log_status_();
  }
}

bool RTSPServer::start_pipelines_() {
  if (this->video_enabled_) {
    const bool ok = this->video_.start(this->video_config_, [this](const uint8_t *au, size_t len, uint32_t ts) {
      this->on_video_frame_(au, len, ts);
    });
    if (!ok) {
      ESP_LOGE(TAG, "the video pipeline failed to start");
      return false;
    }
  }

  if (this->audio_enabled_) {
    const bool ok = this->audio_.start(this->audio_config_, [this](const uint8_t *pcm, size_t n, uint32_t ts) {
      this->on_audio_frame_(pcm, n, ts);
    });
    if (!ok)
      ESP_LOGW(TAG, "the audio pipeline failed to start; the stream will be video only");
  }

  if (!this->open_listener_())
    return false;

  if (xTaskCreatePinnedToCore(RTSPServer::network_task_trampoline_, "rtsp_net", 6144, this, 4,
                              &this->network_task_, 0) != pdPASS) {
    ESP_LOGE(TAG, "failed to create the network task");
    return false;
  }

  ESP_LOGI(TAG, "listening on rtsp://%s:%u%s", default_local_address().c_str(), this->port_,
           this->path_.c_str());
  return true;
}

void RTSPServer::dump_config() {
  ESP_LOGCONFIG(TAG, "RTSP server:");
  ESP_LOGCONFIG(TAG, "  URL: rtsp://%s:%u%s", default_local_address().c_str(), this->port_,
                this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  Authentication: %s", this->auth_token_.empty() ? "disabled" : "basic");
  ESP_LOGCONFIG(TAG, "  Max clients: %u", this->max_clients_);
  ESP_LOGCONFIG(TAG, "  RTP packet size: %u bytes", this->packet_size_);

  // The resolution is whatever the sensor was initialised with, so it is only
  // known once the pipeline opens the device; it is logged there.
  if (!this->video_enabled_) {
    ESP_LOGCONFIG(TAG, "  Video: disabled");
  } else {
    ESP_LOGCONFIG(TAG, "  Video: MJPEG quality %u @ %" PRIu32 " fps", this->video_config_.jpeg_quality,
                  this->video_config_.framerate);
    ESP_LOGCONFIG(TAG, "         WebRTC cannot carry MJPEG: go2rtc will transcode it (see the README)");
  }

  if (this->audio_enabled_) {
    ESP_LOGCONFIG(TAG, "  Audio: %s 8000 Hz mono (I2S at %" PRIu32 " Hz)",
                  this->audio_config_.codec == AudioCodec::PCMU ? "PCMU" : "PCMA", this->audio_config_.sample_rate);
    ESP_LOGCONFIG(TAG, "  Backchannel: %s", this->audio_.has_speaker() ? "enabled (ONVIF)" : "disabled (no speaker)");
    ESP_LOGCONFIG(TAG, "  Half duplex: %s", YESNO(this->audio_config_.half_duplex));
  } else {
    ESP_LOGCONFIG(TAG, "  Audio: disabled");
  }
}

// ---------------------------------------------------------------------------
// Pipeline callbacks (run on the video / audio tasks)
// ---------------------------------------------------------------------------

void RTSPServer::on_video_frame_(const uint8_t *frame, size_t len, uint32_t timestamp) {
  if (this->active_streams_ == 0)
    return;
  if (!this->mjpeg_packetizer_->packetize(frame, len, timestamp, this)) {
    if ((this->jpeg_parse_errors_++ % 100) == 0) {
      ESP_LOGW(TAG,
               "this JPEG frame cannot be described by RFC 2435 (dimensions, subsampling or quantization "
               "tables); dropped %" PRIu32 " so far",
               this->jpeg_parse_errors_);
    }
  }
}

void RTSPServer::on_audio_frame_(const uint8_t *g711, size_t count, uint32_t timestamp) {
  if (this->active_streams_ == 0)
    return;
  this->audio_packetizer_->packetize(g711, count, timestamp, this);
}

void RTSPServer::send_rtp(StreamKind kind, uint8_t *buf, size_t rtp_len) {
  if (this->tx_ring_ == nullptr)
    return;

  buf[0] = '$';
  buf[1] = static_cast<uint8_t>(kind);  // rewritten per session with its channel
  buf[2] = static_cast<uint8_t>(rtp_len >> 8);
  buf[3] = static_cast<uint8_t>(rtp_len);

  // The RTP marker sits on the last packet of a video frame, for both RFC 2435
  // and RFC 6184. (Audio sets it on every packet, hence the kind check below.)
  const bool video = kind == StreamKind::VIDEO;
  const bool last_of_frame = (buf[INTERLEAVED_HEADER_SIZE + 1] & 0x80) != 0;

  // Never block a pipeline task on the network: drop instead. But drop WHOLE
  // frames rather than isolated packets. A JPEG missing a fragment from its
  // middle is not a slightly worse picture: the decoder resynchronises on
  // whatever bytes follow and paints parts of two frames at once, which is the
  // "torn" or "doubled" image seen on the receiver. Truncating at the overflow
  // point and resuming cleanly on the next frame costs frames, not coherence.
  if (video && this->tx_dropping_frame_) {
    this->tx_overflows_++;
    if (last_of_frame)
      this->tx_dropping_frame_ = false;  // the next frame starts clean
    return;
  }

  if (xRingbufferSend(this->tx_ring_, buf, INTERLEAVED_HEADER_SIZE + rtp_len, 0) != pdTRUE) {
    if (video && !last_of_frame) {
      this->tx_dropping_frame_ = true;
      this->tx_frames_dropped_++;
    }
    if ((this->tx_overflows_++ % 100) == 0) {
      ESP_LOGW(TAG,
               "transmit ring buffer full: the link cannot carry the stream. "
               "%" PRIu32 " packets / %" PRIu32 " frames dropped. Lower 'jpeg_quality' or 'framerate'.",
               this->tx_overflows_, this->tx_frames_dropped_);
    }
  }
}

// ---------------------------------------------------------------------------
// Network task
// ---------------------------------------------------------------------------

bool RTSPServer::open_listener_() {
  this->listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->listen_fd_ < 0) {
    ESP_LOGE(TAG, "socket() failed (errno %d)", errno);
    return false;
  }

  int one = 1;
  setsockopt(this->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(this->port_);

  if (bind(this->listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind() to port %u failed (errno %d)", this->port_, errno);
    close(this->listen_fd_);
    this->listen_fd_ = -1;
    return false;
  }

  if (listen(this->listen_fd_, 2) != 0) {
    ESP_LOGE(TAG, "listen() failed (errno %d)", errno);
    close(this->listen_fd_);
    this->listen_fd_ = -1;
    return false;
  }

  return true;
}

void RTSPServer::refresh_negotiated_mask_() {
  // OR across every PLAYing session, not just the last one to arrive. With a
  // transcoding chain the device serves two connections -- one pulling video,
  // one holding the backchannel -- and reporting only the most recent made the
  // backchannel look absent while another session had it.
  uint8_t mask = 0;
  for (const auto &s : this->sessions_) {
    if (!s->playing || s->fd < 0)
      continue;
    for (int k = 0; k < 3; k++)
      if (s->setup[k])
        mask |= static_cast<uint8_t>(1u << k);
  }
  this->negotiated_mask_ = mask;
}

void RTSPServer::log_status_() {
  const uint8_t mask = this->negotiated_mask_;
  const auto yn = [](bool b) { return b ? "yes" : "NO "; };

  ESP_LOGI(TAG, "--- status ------------------------------------------------");
  // The mask is the OR across every PLAYing session, not the last one to
  // arrive: with a transcoding chain the device serves two connections, and
  // reporting only the most recent made the backchannel look absent while
  // another session held it.
  ESP_LOGI(TAG, "  clients=%u playing=%u | negotiated across all sessions: video=%s audio=%s backchannel=%s",
           static_cast<unsigned>(this->client_count_), static_cast<unsigned>(this->active_streams_),
           yn((mask & 0x01) != 0), yn((mask & 0x02) != 0), yn((mask & 0x04) != 0));

  // Why the backchannel is absent, which is a different question from whether
  // it is absent -- and the two have opposite fixes.
  if ((mask & 0x04) == 0) {
    if (this->describes_with_backchannel_ == 0) {
      ESP_LOGI(TAG,
               "         no client has ASKED to talk yet (%" PRIu32 " DESCRIBEs, none with the ONVIF Require "
               "header). Normal until a viewer opens the stream with a microphone -- nothing to fix here.",
               this->describes_total_);
    } else {
      ESP_LOGW(TAG,
               "         %" PRIu32 " client(s) asked for the backchannel but never set the track up: look at "
               "the SDP and at the client, not at the wiring",
               this->describes_with_backchannel_);
    }
  }
  ESP_LOGI(TAG, "  video: %" PRIu32 " encoded, %" PRIu32 " skipped | tx: %" PRIu32 " packets, %" PRIu32
                " frames dropped",
           this->video_.frames_encoded(), this->video_.frames_dropped(), this->tx_overflows_,
           this->tx_frames_dropped_);
  if (this->audio_.is_running()) {
    ESP_LOGI(TAG, "  audio: mic %" PRIu32 " packets sent | backchannel %" PRIu32 " received, %" PRIu32 " dropped%s",
             this->audio_.packets_sent(), this->audio_.packets_received(), this->audio_.packets_dropped(),
             this->audio_.is_talking() ? "  <-- TALKING NOW" : "");

    // The microphone, measured rather than assumed. Three distinct faults look
    // identical from Home Assistant ("I hear nothing") and are told apart here:
    //   samples frozen      -> the source delivers no data at all (wrong pins,
    //                          codec not started, another consumer took it)
    //   samples rising,     -> the source delivers digital silence (mic muted,
    //   level -100 dB          gain at zero, wrong I2S slot, dead capsule)
    //   level around -60 dB -> it works, it is just quiet: raise the gain
    // Two readings, because one of them is almost always misleading on its own.
    // `now` decays in a second, so unless you happen to be speaking at the exact
    // moment this line is printed it shows room tone. `max/60s` is the loudest
    // thing heard in the last minute -- that is the one that separates a quiet
    // room from a microphone that hears nothing.
    ESP_LOGI(TAG, "  mic:   %" PRIu32 " samples read (%s), now %.1f dBFS %s  max/60s %.1f dBFS",
             this->audio_.mic_samples(), this->audio_.mic_alive() ? "flowing" : "STOPPED",
             static_cast<double>(this->audio_.mic_level_db()), this->audio_.mic_level_bar(),
             static_cast<double>(this->audio_.mic_peak_hold_db()));

    // Speech should reach -30..-6 dBFS. Much below that and the far end hears
    // nothing, however healthy every counter above may look.
    const float hold = this->audio_.mic_peak_hold_db();
    if (this->audio_.mic_alive() && hold > -100.0f && hold < -40.0f) {
      ESP_LOGW(TAG,
               "         the loudest sound of the last minute was %.1f dBFS -- far too quiet. Speech should "
               "reach -30..-6 dBFS. Raise the gain (the codec's own first, then 'gain:' here).",
               static_cast<double>(hold));
    }
    if (this->audio_.has_speaker()) {
      ESP_LOGI(TAG, "  spk:   now %.1f dBFS %s  max/60s %.1f dBFS%s",
               static_cast<double>(this->audio_.speaker_level_db()), this->audio_.speaker_level_bar(),
               static_cast<double>(this->audio_.speaker_peak_hold_db()),
               this->audio_.loopback() ? "  <-- LOOPBACK TEST ON" : "");
      // The line that answers "I press the test beep and hear nothing". The
      // speaker reports how much it took; offered without written means the
      // sink refuses the data and no volume setting will ever produce a sound.
      const uint32_t offered = this->audio_.speaker_bytes_offered();
      const char *verdict = "";
      if (offered == 0) {
        // Not a fault, and the distinction matters: nothing has been sent to the
        // speaker, so it has had no chance to fail. Press the test beep before
        // suspecting the output at all.
        verdict = "   (nothing played yet -- press the test beep)";
      } else if (this->audio_.speaker_bytes_written() == 0) {
        verdict = "   <-- THE SPEAKER IS REFUSING EVERYTHING";
      }
      ESP_LOGI(TAG, "         %" PRIu32 " bytes offered, %" PRIu32 " accepted, %" PRIu32 " short writes%s",
               offered, this->audio_.speaker_bytes_written(), this->audio_.speaker_drops(), verdict);
    } else {
      ESP_LOGI(TAG, "  spk:   no speaker configured -- backchannel disabled, nothing can come out");
    }
  } else {
    ESP_LOGI(TAG, "  audio: pipeline not running (no 'audio:' block configured?)");
  }

  // The readings that place a fault without leaving this log:
  //   backchannel=NO      -> the client never asked to talk (card mode, HTTPS,
  //                          or go2rtc source without '#backchannel=1')
  //   frames dropped > 0  -> the link cannot carry the stream: lower
  //                          'jpeg_quality' or 'framerate'
  //   backchannel received stuck at 0 while you press talk -> nothing reaches
  //                          the device; the fault is upstream, not here
  //   mic peak flat at -100 dBFS while you speak -> the fault is on the device,
  //                          upstream of the network; nothing sent will help
  ESP_LOGI(TAG, "-----------------------------------------------------------");
}

void RTSPServer::network_task_trampoline_(void *arg) {
  static_cast<RTSPServer *>(arg)->network_run_();
}

void RTSPServer::network_run_() {
  while (true) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(this->listen_fd_, &read_set);
    int max_fd = this->listen_fd_;

    for (const auto &session : this->sessions_) {
      FD_SET(session->fd, &read_set);
      max_fd = std::max(max_fd, session->fd);
    }

    struct timeval timeout = {};
    timeout.tv_usec = SELECT_TIMEOUT_MS * 1000;

    const int ready = select(max_fd + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready > 0) {
      if (FD_ISSET(this->listen_fd_, &read_set))
        this->accept_client_();

      for (size_t i = this->sessions_.size(); i > 0; i--) {
        RtspSession &session = *this->sessions_[i - 1];
        if (FD_ISSET(session.fd, &read_set))
          this->service_session_(session);
      }
    }

    // Reap sessions closed by service_session_ and time out idle ones.
    const uint32_t now = millis();
    for (size_t i = this->sessions_.size(); i > 0; i--) {
      RtspSession &session = *this->sessions_[i - 1];
      if (session.fd < 0 || (now - session.last_activity_ms) > SESSION_TIMEOUT_MS)
        this->close_session_(i - 1);
    }

    this->refresh_negotiated_mask_();
    this->drain_tx_ring_();
  }
}

void RTSPServer::accept_client_() {
  struct sockaddr_in addr = {};
  socklen_t addr_len = sizeof(addr);
  const int fd = accept(this->listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), &addr_len);
  if (fd < 0)
    return;

  if (this->sessions_.size() >= this->max_clients_) {
    ESP_LOGW(TAG, "refusing %s: already serving %u clients", inet_ntoa(addr.sin_addr), this->max_clients_);
    close(fd);
    return;
  }

  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // Bound how long a stalled client can block the network task.
  struct timeval send_timeout = {};
  send_timeout.tv_sec = 2;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

  auto session = make_unique<RtspSession>();
  session->fd = fd;
  session->last_activity_ms = millis();
  session->authenticated = this->auth_token_.empty();
  this->sessions_.push_back(std::move(session));
  this->client_count_ = static_cast<uint8_t>(this->sessions_.size());

  ESP_LOGI(TAG, "client connected from %s (%u total)", inet_ntoa(addr.sin_addr),
           static_cast<unsigned>(this->sessions_.size()));
}

void RTSPServer::close_session_(size_t index) {
  RtspSession &session = *this->sessions_[index];

  if (session.playing && this->active_streams_ > 0)
    this->active_streams_--;
  if (session.fd >= 0)
    close(session.fd);

  this->sessions_.erase(this->sessions_.begin() + index);
  this->client_count_ = static_cast<uint8_t>(this->sessions_.size());
  ESP_LOGI(TAG, "client disconnected (%u remaining)", static_cast<unsigned>(this->sessions_.size()));
}

void RTSPServer::service_session_(RtspSession &session) {
  uint8_t buffer[1024];
  const ssize_t received = recv(session.fd, buffer, sizeof(buffer), 0);
  if (received <= 0) {
    close(session.fd);
    session.fd = -1;
    return;
  }

  session.last_activity_ms = millis();
  session.rx.append(reinterpret_cast<const char *>(buffer), static_cast<size_t>(received));

  // The same socket carries RTSP requests and, once playing, interleaved RTP
  // from the client's backchannel. Both are demultiplexed here.
  while (!session.rx.empty()) {
    if (session.rx[0] == '$') {
      if (session.rx.size() < INTERLEAVED_HEADER_SIZE)
        break;
      const uint8_t channel = static_cast<uint8_t>(session.rx[1]);
      const size_t length = (static_cast<uint8_t>(session.rx[2]) << 8) | static_cast<uint8_t>(session.rx[3]);
      if (session.rx.size() < INTERLEAVED_HEADER_SIZE + length)
        break;

      this->handle_interleaved_(session, channel,
                                reinterpret_cast<const uint8_t *>(session.rx.data()) + INTERLEAVED_HEADER_SIZE,
                                length);
      session.rx.erase(0, INTERLEAVED_HEADER_SIZE + length);
      continue;
    }

    const size_t header_end = session.rx.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      if (session.rx.size() > 8192) {
        ESP_LOGW(TAG, "oversized request, dropping the client");
        close(session.fd);
        session.fd = -1;
      }
      break;
    }

    const size_t header_len = header_end + 4;
    const std::string headers = session.rx.substr(0, header_len);
    const std::string content_length = header_value(headers, "Content-Length");
    const size_t body_len = content_length.empty() ? 0 : static_cast<size_t>(atoi(content_length.c_str()));
    if (session.rx.size() < header_len + body_len)
      break;

    this->handle_request_(session, headers);
    session.rx.erase(0, header_len + body_len);

    if (session.fd < 0)
      break;
  }
}

void RTSPServer::handle_interleaved_(RtspSession &session, uint8_t channel, const uint8_t *payload, size_t len) {
  // Only the backchannel carries client-to-server RTP; anything else is RTCP
  // that we deliberately ignore.
  //
  // These three rejections used to be silent, which made "I press talk and
  // nothing comes out" impossible to place: the device could not say whether it
  // received nothing at all, or received and discarded. They now leave a trace,
  // rate-limited because RTCP arrives regularly on a healthy session.
  if (!session.setup[static_cast<int>(StreamKind::BACKCHANNEL)]) {
    if ((this->stray_rtp_++ % 200) == 0) {
      ESP_LOGW(TAG,
               "incoming RTP on channel %u but no backchannel was set up: the client never asked for it "
               "(missing 'Require: www.onvif.org/ver20/backchannel' on DESCRIBE)",
               channel);
    }
    return;
  }
  if (channel != session.channel[static_cast<int>(StreamKind::BACKCHANNEL)])
    return;  // RTCP for the outgoing tracks, expected and uninteresting
  if (len <= RTP_HEADER_SIZE)
    return;

  const uint8_t csrc_count = static_cast<uint8_t>(payload[0] & 0x0F);
  size_t offset = RTP_HEADER_SIZE + static_cast<size_t>(csrc_count) * 4;

  if ((payload[0] & 0x10) != 0) {  // extension header present
    if (offset + 4 > len)
      return;
    const size_t extension_words = (static_cast<size_t>(payload[offset + 2]) << 8) | payload[offset + 3];
    offset += 4 + extension_words * 4;
  }
  if (offset >= len)
    return;

  // One line at the start of each talk burst. `is_talking()` is still false
  // until play_g711 records this packet, so this fires once per burst rather
  // than on every 20 ms packet. C'est LA preuve, cote appareil, que la voix
  // descendante arrive jusqu'ici -- sans avoir a interroger le navigateur.
  if (!this->audio_.is_talking())
    ESP_LOGI(TAG, "backchannel: incoming audio from the client (%u byte payload)",
             static_cast<unsigned>(len - offset));

  this->audio_.play_g711(payload + offset, len - offset);
}

// ---------------------------------------------------------------------------
// RTSP methods
// ---------------------------------------------------------------------------

bool RTSPServer::check_auth_(const RtspSession &session, const std::string &request) const {
  if (this->auth_token_.empty())
    return true;
  if (session.authenticated)
    return true;
  return header_value(request, "Authorization") == this->auth_token_;
}

void RTSPServer::handle_request_(RtspSession &session, const std::string &request) {
  const size_t method_end = request.find(' ');
  if (method_end == std::string::npos)
    return;
  const std::string method = request.substr(0, method_end);

  const size_t url_end = request.find(' ', method_end + 1);
  if (url_end == std::string::npos)
    return;
  const std::string url = request.substr(method_end + 1, url_end - method_end - 1);

  const int cseq = parse_cseq(request);
  ESP_LOGD(TAG, "%s %s (CSeq %d)", method.c_str(), url.c_str(), cseq);

  if (method == "OPTIONS") {
    this->send_simple_(session, 200, "OK", cseq,
                       "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n");
    return;
  }

  if (!this->check_auth_(session, request)) {
    this->send_simple_(session, 401, "Unauthorized", cseq, "WWW-Authenticate: Basic realm=\"ESPHome\"\r\n");
    return;
  }
  session.authenticated = true;

  // Every method below addresses the stream, so enforce the configured path.
  if (url != "*" && this->path_ != "/") {
    const size_t scheme = url.find("://");
    const size_t path_start = (scheme == std::string::npos) ? 0 : url.find('/', scheme + 3);
    if (path_start != std::string::npos && url.compare(path_start, this->path_.size(), this->path_) != 0) {
      ESP_LOGW(TAG, "rejecting '%s': the configured path is '%s'", url.c_str(), this->path_.c_str());
      this->send_simple_(session, 404, "Not Found", cseq);
      return;
    }
  }

  if (method == "DESCRIBE") {
    const std::string require = to_lower(header_value(request, "Require"));
    const bool asked = require.find("www.onvif.org/ver20/backchannel") != std::string::npos;
    // `backchannel: always` announces the sendonly track to everyone, so that a
    // client which only probes the stream can still SEE that this camera can be
    // talked to. Without it the capability exists but is undiscoverable, and
    // every card that asks "does it do two-way audio?" answers no.
    session.wants_backchannel = (asked || this->always_advertise_backchannel_) && this->audio_.has_speaker();

    // Counted separately so the status block can distinguish the two ways the
    // talk path stays silent, which look identical from Home Assistant:
    //   describes_with_backchannel_ == 0 -> nobody ever ASKED to talk. The
    //     device is fine; go2rtc only requests the backchannel when a consumer
    //     actually wants to send audio, so this is normal until a browser opens
    //     the stream with a microphone.
    //   asked, but the track is never SETUP -> the client asked and gave up,
    //     which points at the SDP or at the client.
    this->describes_total_++;
    if (asked) {
      this->describes_with_backchannel_++;
      if (!this->audio_.has_speaker())
        ESP_LOGW(TAG, "a client asked for the ONVIF backchannel but no speaker is configured");
      else
        ESP_LOGI(TAG, "backchannel requested by the client; announcing the sendonly track");
    }

    const std::string local_ip = this->local_ip_of_(session.fd);
    const std::string sdp = this->build_sdp_(local_ip, session.wants_backchannel);

    char base[160];
    snprintf(base, sizeof(base), "rtsp://%s:%u%s/", local_ip.c_str(), this->port_, this->path_.c_str());

    char head[512];
    snprintf(head, sizeof(head),
             "RTSP/1.0 200 OK\r\nCSeq: %d\r\nServer: %s\r\nContent-Base: %s\r\nContent-Type: "
             "application/sdp\r\nContent-Length: %u\r\n\r\n",
             cseq, SERVER_NAME, base, static_cast<unsigned>(sdp.size()));
    this->send_response_(session, std::string(head) + sdp);
    return;
  }

  if (method == "SETUP") {
    const std::string transport = header_value(request, "Transport");
    if (to_lower(transport).find("rtp/avp/tcp") == std::string::npos) {
      // UDP is deliberately not implemented: go2rtc uses TCP by default and it
      // is the only transport that keeps the backchannel on one socket.
      ESP_LOGW(TAG, "rejecting non-TCP transport '%s'; use RTP/AVP/TCP (interleaved)", transport.c_str());
      this->send_simple_(session, 461, "Unsupported Transport", cseq);
      return;
    }

    // A SETUP without an explicit trackID addresses the first media, i.e. video.
    const int track = std::max<int>(0, parse_track_id(url));
    if (track > 2) {
      this->send_simple_(session, 454, "Session Not Found", cseq);
      return;
    }

    // Honour the channels the client asked for; that is what it will listen on.
    int channel = track * 2;
    const size_t pos = to_lower(transport).find("interleaved=");
    if (pos != std::string::npos)
      channel = atoi(transport.c_str() + pos + 12);

    session.setup[track] = true;
    session.channel[track] = static_cast<uint8_t>(channel);

    if (session.session_id.empty()) {
      char id[24];
      snprintf(id, sizeof(id), "%08" PRIx32 "%04" PRIx32, random_uint32(), ++this->session_counter_);
      session.session_id = id;
    }

    char extra[192];
    snprintf(extra, sizeof(extra), "Session: %s;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n",
             session.session_id.c_str(), channel, channel + 1);
    this->send_simple_(session, 200, "OK", cseq, extra);
    return;
  }

  if (method == "PLAY") {
    if (session.session_id.empty()) {
      this->send_simple_(session, 454, "Session Not Found", cseq);
      return;
    }
    if (!session.playing) {
      session.playing = true;
      this->active_streams_++;
    }

    this->refresh_negotiated_mask_();

    char extra[96];
    snprintf(extra, sizeof(extra), "Session: %s\r\nRange: npt=0.000-\r\n", session.session_id.c_str());
    this->send_simple_(session, 200, "OK", cseq, extra);
    ESP_LOGI(TAG, "streaming to a client (%u active)", static_cast<unsigned>(this->active_streams_));
    return;
  }

  if (method == "PAUSE") {
    if (session.playing) {
      session.playing = false;
      if (this->active_streams_ > 0)
        this->active_streams_--;
    }
    this->send_simple_(session, 200, "OK", cseq);
    return;
  }

  if (method == "TEARDOWN") {
    this->send_simple_(session, 200, "OK", cseq);
    close(session.fd);
    session.fd = -1;
    return;
  }

  if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
    // Used as a keepalive by most clients; an empty 200 is the right answer.
    this->send_simple_(session, 200, "OK", cseq);
    return;
  }

  this->send_simple_(session, 501, "Not Implemented", cseq);
}

std::string RTSPServer::build_sdp_(const std::string &local_ip, bool with_backchannel) {
  std::string sdp;
  sdp += "v=0\r\n";
  sdp += "o=- 0 0 IN IP4 " + local_ip + "\r\n";
  sdp += "s=ESPHome Doorbell\r\n";
  sdp += "c=IN IP4 0.0.0.0\r\n";
  sdp += "t=0 0\r\n";
  sdp += "a=tool:esphome-rtsp\r\n";
  sdp += "a=control:*\r\n";

  if (this->video_enabled_) {
    // RFC 2435: JPEG is a static payload type, and everything a decoder needs
    // (size, subsampling, quantization tables) travels in the RTP payload
    // header, so the media description carries no format parameters at all.
    sdp += "m=video 0 RTP/AVP 26\r\n";
    sdp += "a=rtpmap:26 JPEG/90000\r\n";
    sdp += "a=control:trackID=0\r\n";
  }

  if (this->audio_enabled_) {
    const bool ulaw = this->audio_config_.codec == AudioCodec::PCMU;
    const char *name = ulaw ? "PCMU" : "PCMA";
    const int payload_type = ulaw ? 0 : 8;

    char buf[96];
    snprintf(buf, sizeof(buf), "m=audio 0 RTP/AVP %d\r\n", payload_type);
    sdp += buf;
    snprintf(buf, sizeof(buf), "a=rtpmap:%d %s/8000\r\n", payload_type, name);
    sdp += buf;
    sdp += "a=control:trackID=1\r\n";

    if (with_backchannel) {
      // ONVIF Streaming Specification §5.3: the audio output of the device is
      // advertised as a separate `sendonly` media description, which is what
      // go2rtc looks for when it asks for the backchannel.
      snprintf(buf, sizeof(buf), "m=audio 0 RTP/AVP %d\r\n", payload_type);
      sdp += buf;
      snprintf(buf, sizeof(buf), "a=rtpmap:%d %s/8000\r\n", payload_type, name);
      sdp += buf;
      sdp += "a=control:trackID=2\r\n";
      sdp += "a=sendonly\r\n";
    }
  }

  return sdp;
}

std::string RTSPServer::stream_url() const {
  char buf[96];
  snprintf(buf, sizeof(buf), "rtsp://%s:%u%s", default_local_address().c_str(), this->port_,
           this->path_.c_str());
  return std::string(buf);
}

std::string RTSPServer::local_ip_of_(int fd) const {
  struct sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) == 0)
    return std::string(inet_ntoa(addr.sin_addr));
  return default_local_address();
}

// ---------------------------------------------------------------------------
// Transmission
// ---------------------------------------------------------------------------

void RTSPServer::send_simple_(RtspSession &session, int code, const char *reason, int cseq,
                              const std::string &extra) {
  char head[256];
  snprintf(head, sizeof(head), "RTSP/1.0 %d %s\r\nCSeq: %d\r\nServer: %s\r\n", code, reason, cseq, SERVER_NAME);
  this->send_response_(session, std::string(head) + extra + "\r\n");
}

void RTSPServer::send_response_(RtspSession &session, const std::string &response) {
  if (!this->send_all_(session.fd, reinterpret_cast<const uint8_t *>(response.data()), response.size())) {
    close(session.fd);
    session.fd = -1;
  }
}

bool RTSPServer::send_all_(int fd, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const ssize_t n = send(fd, data + sent, len - sent, 0);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR))
        continue;
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

void RTSPServer::drain_tx_ring_() {
  while (true) {
    size_t item_size = 0;
    uint8_t *item = static_cast<uint8_t *>(xRingbufferReceive(this->tx_ring_, &item_size, 0));
    if (item == nullptr)
      break;

    const int kind = static_cast<int>(item[1]);
    for (auto &session : this->sessions_) {
      if (!session->playing || session->fd < 0 || kind < 0 || kind > 2)
        continue;
      if (!session->setup[kind])
        continue;

      // The interleaved channel is per session, so patch it in place.
      item[1] = session->channel[kind];
      if (!this->send_all_(session->fd, item, item_size)) {
        ESP_LOGW(TAG, "write failed, dropping the client");
        close(session->fd);
        session->fd = -1;
      }
    }

    vRingbufferReturnItem(this->tx_ring_, item);
  }
}

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
