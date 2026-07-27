#pragma once
#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>

// Stub of esphome/core/automation.h.
//
// The signatures below are copied from the real header ON PURPOSE, character
// for character: a stub that is more permissive than reality turns a green lint
// into a false sense of safety. `tests/check_esphome_api.py` re-checks them
// against the installed ESPHome package, so drift is caught rather than
// inherited.
namespace esphome {

template<typename... Ts> class Trigger { public: void trigger(Ts... x) {} };

/// Function-pointer-only storage, as used by TEMPLATABLE_VALUE for trivially
/// copyable types. It deliberately REFUSES raw constants: codegen must wrap
/// them with `cg.templatable(...)`, which emits a stateless lambda.
template<typename T, typename... X> class TemplatableFn {
 public:
  TemplatableFn() = default;
  template<typename F> TemplatableFn(F f) : f_(f) {}
  T value(X... x) const { return this->f_ ? this->f_(x...) : T{}; }

 protected:
  T (*f_)(X...){nullptr};
};

/// Value-or-function storage, used for non-trivially-copyable types. This one
/// does accept raw constants.
template<typename T, typename... X> class TemplatableValue {
 public:
  TemplatableValue() = default;
  template<typename V> TemplatableValue(V value) : value_(static_cast<T>(value)) {}
  T value(X... x) const { return this->value_; }

 protected:
  T value_{};
};

template<typename T, typename... X>
using TemplatableStorage =
    std::conditional_t<std::is_trivially_copyable_v<T>, TemplatableFn<T, X...>, TemplatableValue<T, X...>>;

template<typename... Ts> class Action {
 public:
  virtual ~Action() = default;
  // `const Ts &...`, exactly as upstream. An override written `Ts...` does not
  // override anything, and the class silently stays abstract.
  virtual void play(const Ts &...x) = 0;
};

}  // namespace esphome

#define TEMPLATABLE_VALUE(type, name) \
 protected: \
  esphome::TemplatableStorage<type, Ts...> name##_{}; \
\
 public: \
  template<typename V> void set_##name(V name) { this->name##_ = name; }
