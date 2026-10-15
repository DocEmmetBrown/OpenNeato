// Ported from OpenNeato firmware/src/async_cache.h (MIT)
//
// Generic async cache with TTL, request coalescing and explicit invalidation.
// Pure C++17; time comes from an injected clock function.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace neatocore {

using ClockFn = uint32_t (*)();

template<typename T> class AsyncCache {
 public:
  using Callback = std::function<void(bool, const T &)>;
  using FetchFunc = std::function<void(Callback)>;

  AsyncCache(ClockFn clock, uint32_t ttl_ms, FetchFunc fetch) : clock_(clock), ttl_(ttl_ms), fetch_(std::move(fetch)) {}

  // Cached value if fresh, else fetch (or piggyback on the fetch in flight).
  void get(Callback callback) {
    if (this->has_value_ && this->clock_() - this->cached_at_ < this->ttl_) {
      if (callback)
        callback(true, this->cached_);
      return;
    }
    if (callback)
      this->waiters_.push_back(std::move(callback));
    if (this->fetching_)
      return;
    this->start_fetch_();
  }

  // Always fetch fresh (still coalesced with a fetch in flight).
  void refresh(Callback callback) {
    this->has_value_ = false;
    this->get(std::move(callback));
  }

  void invalidate() { this->has_value_ = false; }
  bool has_cached() const { return this->has_value_; }
  bool is_fetching() const { return this->fetching_; }
  const T &cached() const { return this->cached_; }
  void set_ttl(uint32_t ttl_ms) { this->ttl_ = ttl_ms; }

 protected:
  void start_fetch_() {
    this->fetching_ = true;
    this->fetch_([this](bool ok, const T &data) {
      this->fetching_ = false;
      if (ok) {
        this->cached_ = data;
        this->cached_at_ = this->clock_();
        this->has_value_ = true;
      }
      // Move out first: a waiter may call get() again.
      auto pending = std::move(this->waiters_);
      this->waiters_.clear();
      for (auto &cb : pending)
        cb(ok, ok ? this->cached_ : data);
    });
  }

  ClockFn clock_;
  uint32_t ttl_;
  FetchFunc fetch_;
  T cached_{};
  uint32_t cached_at_{0};
  bool has_value_{false};
  bool fetching_{false};
  std::vector<Callback> waiters_;
};

}  // namespace neatocore
