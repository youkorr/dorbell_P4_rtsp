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

/// `rtsp_server.set_loopback` — route the microphone to the speaker locally.
///
/// The bench test for "is the audio working at all": press it, speak, and if
/// you hear yourself then capture, gain, companding and playback are all fine
/// and any remaining fault is in the network or in Home Assistant.
template<typename... Ts> class SetLoopbackAction : public Action<Ts...> {
 public:
  explicit SetLoopbackAction(RTSPServer *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, state)

  void play(Ts... x) override { this->parent_->set_audio_loopback(this->state_.value(x...)); }

 protected:
  RTSPServer *parent_;
};

/// `rtsp_server.play_test_tone` — beep on the speaker, to prove the output
/// path on its own when the loopback test says nothing.
template<typename... Ts> class PlayTestToneAction : public Action<Ts...> {
 public:
  explicit PlayTestToneAction(RTSPServer *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint32_t, frequency)
  TEMPLATABLE_VALUE(uint32_t, duration)

  void play(Ts... x) override {
    this->parent_->play_test_tone(this->frequency_.value(x...), this->duration_.value(x...));
  }

 protected:
  RTSPServer *parent_;
};

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
