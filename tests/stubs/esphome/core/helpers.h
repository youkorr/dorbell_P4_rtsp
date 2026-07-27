#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
namespace esphome {
using std::make_unique;
uint32_t random_uint32();
class Mutex { public: void lock(); void unlock(); };
class LockGuard { public: explicit LockGuard(Mutex &m) : m_(m) { m_.lock(); } ~LockGuard() { m_.unlock(); } private: Mutex &m_; };
template<typename... X> class CallbackManager;
template<typename... Ts> class CallbackManager<void(Ts...)> {
 public:
  void add(std::function<void(Ts...)> &&cb) { cbs_.push_back(std::move(cb)); }
  void call(Ts... args) { for (auto &cb : cbs_) cb(args...); }
 private:
  std::vector<std::function<void(Ts...)>> cbs_;
};
}
