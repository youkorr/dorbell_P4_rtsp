#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "esphome/components/audio/audio.h"
namespace esphome { namespace microphone {
class Microphone {
 public:
  virtual void start() = 0;
  virtual void stop() = 0;
  template<typename F> void add_data_callback(F &&cb) { cb_ = cb; }
  bool is_running() const { return false; }
 private:
  std::function<void(const std::vector<uint8_t> &)> cb_;
};
} }
