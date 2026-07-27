#pragma once
#include <functional>
#include <tuple>
namespace esphome {

template<typename... Ts> class Trigger { public: void trigger(Ts... x) {} };

/// Mirrors esphome/core/automation.h: a value that is either a constant or a
/// lambda evaluated with the automation's arguments.
template<typename T, typename... X> class TemplatableValue {
 public:
  TemplatableValue() = default;
  TemplatableValue(T value) : value_(value) {}
  template<typename F> TemplatableValue(F f) : f_(f), templated_(true) {}
  T value(X... x) { return this->templated_ ? this->f_(x...) : this->value_; }

 protected:
  T value_{};
  std::function<T(X...)> f_{};
  bool templated_{false};
};

template<typename... Ts> class Action {
 public:
  virtual ~Action() = default;
  virtual void play(Ts... x) = 0;
};

}  // namespace esphome

#define TEMPLATABLE_VALUE(type, name) \
 protected: \
  esphome::TemplatableValue<type, Ts...> name##_{}; \
\
 public: \
  template<typename V> void set_##name(V name) { this->name##_ = name; }
