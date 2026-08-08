// Instantiate the actions the way ESPHome's generated main.cpp does.
//
// Compiling automation.h on its own proves nothing about the action classes:
// they are templates, so `override` mismatches and leftover pure virtuals are
// only diagnosed when a concrete instantiation is created. That is exactly how
// `void play(Ts... x) override` -- which overrides nothing, because upstream
// declares `play(const Ts &...x)` -- reached a user's build. The class stayed
// abstract, and the error was reported against the `new` in main.cpp, i.e.
// against a line of YAML with no obvious link to this component.
//
// So: `new` each action here, through a base-class pointer, with the parameter
// packs ESPHome actually uses.

#include "automation.h"

using namespace esphome;
using namespace esphome::rtsp_server;

// A bare automation (`button` on_press, `switch` turn_on_action): no arguments.
template class esphome::rtsp_server::SetLoopbackAction<>;
template class esphome::rtsp_server::PlayTestToneAction<>;

// An LVGL widget's on_click passes <bool, lv_event_t *>; a two-argument pack is
// what caught the abstract-class bug, so keep one here.
struct FakeEvent;
template class esphome::rtsp_server::SetLoopbackAction<bool, FakeEvent *>;
template class esphome::rtsp_server::PlayTestToneAction<bool, FakeEvent *>;

int main() {
  RTSPServer *parent = nullptr;

  // Through Action<...> *, so a class that is still abstract cannot compile.
  Action<> *a = new SetLoopbackAction<>(parent);
  Action<> *b = new PlayTestToneAction<>(parent);
  Action<bool, FakeEvent *> *c = new SetLoopbackAction<bool, FakeEvent *>(parent);
  Action<bool, FakeEvent *> *d = new PlayTestToneAction<bool, FakeEvent *>(parent);

  // The setters codegen emits, with the shapes codegen emits them in: a
  // stateless lambda for a templatable field, a plain scalar for a plain one.
  static_cast<SetLoopbackAction<> *>(a)->set_state([]() -> bool { return true; });
  static_cast<PlayTestToneAction<> *>(b)->set_frequency([]() -> uint32_t { return 1000; });
  static_cast<PlayTestToneAction<> *>(b)->set_duration(600);

  return (a && b && c && d) ? 0 : 1;
}
