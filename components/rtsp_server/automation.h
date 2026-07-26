#pragma once

// Pulls in USE_ESP32 before it is tested below.
#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "rtsp_server.h"

namespace esphome {
namespace rtsp_server {

/// Fires when a client opens an RTSP connection.
class ClientConnectedTrigger : public Trigger<> {
 public:
  explicit ClientConnectedTrigger(RTSPServer *parent) {
    parent->add_on_client_connected_callback([this]() { this->trigger(); });
  }
};

/// Fires when a client disconnects.
class ClientDisconnectedTrigger : public Trigger<> {
 public:
  explicit ClientDisconnectedTrigger(RTSPServer *parent) {
    parent->add_on_client_disconnected_callback([this]() { this->trigger(); });
  }
};

/// Fires when backchannel audio starts arriving, i.e. someone in Home Assistant
/// pressed push-to-talk. Useful to unmute an amplifier or light up an LED.
class TalkStartTrigger : public Trigger<> {
 public:
  explicit TalkStartTrigger(RTSPServer *parent) {
    parent->add_on_talk_start_callback([this]() { this->trigger(); });
  }
};

/// Fires once the backchannel has been silent for `talk_timeout`.
class TalkEndTrigger : public Trigger<> {
 public:
  explicit TalkEndTrigger(RTSPServer *parent) {
    parent->add_on_talk_end_callback([this]() { this->trigger(); });
  }
};

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
