#pragma once
#include <tuple>
namespace esphome { template<typename... Ts> class Trigger { public: void trigger(Ts... x) {} }; }
