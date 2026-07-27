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

  // `const Ts &...` and not `Ts...`: that is the signature declared in
  // esphome/core/automation.h. Get it wrong and this does not override anything,
  // so the class stays abstract and the error lands on `new` at the call site,
  // pointing at a line of YAML rather than at this file.
  void play(const Ts &...x) override { this->parent_->set_audio_loopback(this->state_.value(x...)); }

 protected:
  RTSPServer *parent_;
};

/// `rtsp_server.play_test_tone` — beep on the speaker, to prove the output
/// path on its own when the loopback test says nothing.
template<typename... Ts> class PlayTestToneAction : public Action<Ts...> {
 public:
  explicit PlayTestToneAction(RTSPServer *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint32_t, frequency)

  /// Plain, not TEMPLATABLE_VALUE: `duration` is validated as a time period,
  /// which is never a lambda, so templatable storage would buy nothing -- and it
  /// would have to be fed through `cg.templatable()` in the codegen, since
  /// TemplatableFn keeps only a function pointer and rejects raw constants.
  void set_duration(uint32_t duration_ms) { this->duration_ms_ = duration_ms; }

  void play(const Ts &...x) override {
    this->parent_->play_test_tone(this->frequency_.value(x...), this->duration_ms_);
  }

 protected:
  RTSPServer *parent_;
  uint32_t duration_ms_{500};
};

}  // namespace rtsp_server
}  // namespace esphome

#endif  // USE_ESP32
