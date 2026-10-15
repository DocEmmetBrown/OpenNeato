// Ported from OpenNeato firmware/src/manual_clean_manager.{h,cpp} (MIT)
//
// Manual drive: TestMode + LDS lifecycle, bumper/wheel-lift safety polling,
// wheel-load stall detection and a 5 s move watchdog. Pure C++17 on top of
// NeatoClient.
#pragma once

#include <cstdint>
#include <functional>

#include "neato_client.h"

namespace neatocore {

static const uint32_t MANUAL_SAFETY_POLL_MS = 500;
static const uint32_t MANUAL_STALL_POLL_MS = 500;
static const int MANUAL_STALL_COUNT = 6;  // consecutive overloaded polls (3 s)
static const uint32_t MANUAL_MOVE_TIMEOUT_MS = 5000;
static const uint32_t MANUAL_ENABLE_TIMEOUT_MS = 10000;

struct SafetyState {
  bool bumper_front_left = false;
  bool bumper_front_right = false;
  bool bumper_side_left = false;
  bool bumper_side_right = false;
  bool wheel_lifted = false;
  bool stall_front = false;
  bool stall_rear = false;
};

// Movement rule from the legacy manager: returns false when the move is
// blocked; may clear stall flags when escaping in the opposite direction.
bool is_move_allowed(SafetyState &s, int left_mm, int right_mm);

// One stall-detection poll; returns true when the wheels must stop.
bool stall_step(SafetyState &s, int &stall_count, int left_cmd_mm, int right_cmd_mm, int left_load, int right_load,
                int threshold_pct);

enum class MoveResult : uint8_t { OK, INACTIVE, BLOCKED, QUEUE_FULL };

class ManualController {
 public:
  explicit ManualController(NeatoClient *client) : client_(client) {}

  // TestMode On -> SetLDSRotation On, or stop wheels -> motors off -> LDS off -> TestMode Off.
  bool enable(bool on, uint32_t now, DoneCallback cb = nullptr);
  MoveResult move(int left_mm, int right_mm, int speed_mm_s, uint32_t now);
  bool set_motors(bool brush, bool vacuum, bool side_brush, DoneCallback cb = nullptr);
  void tick(uint32_t now);

  bool active() const { return this->active_; }
  bool transitioning() const { return this->enabling_ || this->disabling_; }
  bool brush_on() const { return this->brush_on_; }
  bool vacuum_on() const { return this->vacuum_on_; }
  bool side_brush_on() const { return this->side_brush_on_; }
  const SafetyState &safety() const { return this->safety_; }

  void set_stall_threshold(int pct) { this->stall_pct_ = pct; }
  void set_brush_rpm(int rpm) { this->brush_rpm_ = rpm; }
  void set_vacuum_speed(int pct) { this->vacuum_pct_ = pct; }
  void set_side_brush_power(int mw) { this->side_brush_mw_ = mw; }
  // Fired whenever active/motor/safety state changes.
  void on_change(std::function<void()> cb) { this->change_cb_ = std::move(cb); }

 protected:
  void poll_bumpers_();
  void poll_stall_();
  void stop_wheels_();
  void stop_all_motors_();
  void changed_() {
    if (this->change_cb_)
      this->change_cb_();
  }

  NeatoClient *client_;
  bool active_{false};
  bool enabling_{false};
  bool disabling_{false};
  uint32_t enabling_since_{0};
  bool brush_on_{false};
  bool vacuum_on_{false};
  bool side_brush_on_{false};
  SafetyState safety_;
  bool wheels_moving_{false};
  int last_left_mm_{0};
  int last_right_mm_{0};
  int stall_count_{0};
  uint32_t last_move_ms_{0};
  uint32_t last_safety_poll_{0};
  uint32_t last_stall_poll_{0};
  bool safety_poll_in_flight_{false};
  bool stall_poll_in_flight_{false};
  int stall_pct_{80};
  int brush_rpm_{1200};
  int vacuum_pct_{80};
  int side_brush_mw_{1500};
  std::function<void()> change_cb_;
};

}  // namespace neatocore
