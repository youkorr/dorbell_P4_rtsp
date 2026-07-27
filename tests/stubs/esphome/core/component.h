#pragma once
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
namespace esphome {
namespace setup_priority { extern const float AFTER_CONNECTION; extern const float DATA; }
class Component {
 public:
  virtual void setup();
  virtual void loop();
  virtual void dump_config();
  virtual float get_setup_priority() const;
  void mark_failed();
};
}
