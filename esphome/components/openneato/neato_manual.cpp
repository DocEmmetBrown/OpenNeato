// Ported from OpenNeato firmware/src/manual_clean_manager.cpp (MIT)
#include "neato_manual.h"

#include <memory>

namespace neatocore {

bool is_move_allowed(SafetyState &s, int left_mm, int right_mm) {
  if (s.wheel_lifted)
    return false;
  bool forward = left_mm > 0 || right_mm > 0;
  bool backward = left_mm < 0 || right_mm < 0;
  bool turning_left = right_mm > left_mm;
  bool turning_right = left_mm > right_mm;

  bool front_blocked = s.bumper_front_left || s.bumper_front_right || s.stall_front;
  bool rear_blocked = s.stall_rear;

  // Escape moves clear the stall flag of the opposite side.
  if (backward && !forward && s.stall_front)
    s.stall_front = false;
  if (forward && !backward && s.stall_rear)
    s.stall_rear = false;

  if (front_blocked && forward)
    return false;
  if (s.bumper_front_left && turning_right && !backward)
    return false;
  if (s.bumper_front_right && turning_left && !backward)
    return false;
  if (rear_blocked && backward)
    return false;
  if (s.bumper_side_left && turning_right && !backward)
    return false;
  if (s.bumper_side_right && turning_left && !backward)
    return false;
  return true;
}

bool stall_step(SafetyState &s, int &stall_count, int left_cmd_mm, int right_cmd_mm, int left_load, int right_load,
                int threshold_pct) {
  bool overloaded = (left_cmd_mm != 0 && left_load >= threshold_pct) || (right_cmd_mm != 0 && right_load >= threshold_pct);
  if (!overloaded) {
    stall_count = 0;
    return false;
  }
  stall_count++;
  if (stall_count < MANUAL_STALL_COUNT)
    return false;
  // Virtual bumpers in the commanded direction, cleared by an escape move.
  if (left_cmd_mm > 0 || right_cmd_mm > 0)
    s.stall_front = true;
  if (left_cmd_mm < 0 || right_cmd_mm < 0)
    s.stall_rear = true;
  return true;
}

bool ManualController::enable(bool on, uint32_t now, DoneCallback cb) {
  if (on) {
    if (this->active_ || this->enabling_) {
      if (cb)
        cb(false);
      return false;
    }
    this->enabling_ = true;
    this->enabling_since_ = now;
    this->client_->test_mode(true, [this, cb](bool ok) {
      if (!ok) {
        this->enabling_ = false;
        this->changed_();
        if (cb)
          cb(false);
        return;
      }
      this->client_->set_lds_rotation(true, [this, cb](bool ok2) {
        if (!ok2) {
          this->client_->test_mode(false, nullptr);
          this->enabling_ = false;
          this->changed_();
          if (cb)
            cb(false);
          return;
        }
        this->enabling_ = false;
        this->active_ = true;
        this->client_->set_manual_clean_active(true);
        this->safety_ = SafetyState{};
        this->wheels_moving_ = false;
        this->stall_count_ = 0;
        this->brush_on_ = this->vacuum_on_ = this->side_brush_on_ = false;
        this->last_safety_poll_ = 0;
        this->changed_();
        if (cb)
          cb(true);
      });
    });
    return true;
  }

  if (!this->active_ || this->disabling_) {
    if (cb)
      cb(false);
    return false;
  }
  this->disabling_ = true;
  this->client_->set_motor_wheels(0, 0, 0, [this, cb](bool) {
    this->wheels_moving_ = false;
    this->stop_all_motors_();
    this->client_->set_lds_rotation(false, [this, cb](bool) {
      this->client_->test_mode(false, [this, cb](bool ok) {
        this->active_ = false;
        this->disabling_ = false;
        this->client_->set_manual_clean_active(false);
        this->changed_();
        if (cb)
          cb(ok);
      });
    });
  });
  return true;
}

MoveResult ManualController::move(int left_mm, int right_mm, int speed_mm_s, uint32_t now) {
  if (!this->active_ || this->disabling_)
    return MoveResult::INACTIVE;
  this->last_move_ms_ = now;
  if (left_mm == 0 && right_mm == 0) {
    this->wheels_moving_ = false;
    return this->client_->set_motor_wheels(0, 0, 0) ? MoveResult::OK : MoveResult::QUEUE_FULL;
  }
  bool stalls_before = this->safety_.stall_front || this->safety_.stall_rear;
  if (!is_move_allowed(this->safety_, left_mm, right_mm)) {
    this->stop_wheels_();
    return MoveResult::BLOCKED;
  }
  if (stalls_before != (this->safety_.stall_front || this->safety_.stall_rear))
    this->changed_();
  this->last_left_mm_ = left_mm;
  this->last_right_mm_ = right_mm;
  if (!this->wheels_moving_) {
    this->wheels_moving_ = true;
    this->stall_count_ = 0;
    this->last_stall_poll_ = now;
  }
  return this->client_->set_motor_wheels(left_mm, right_mm, speed_mm_s) ? MoveResult::OK : MoveResult::QUEUE_FULL;
}

bool ManualController::set_motors(bool brush, bool vacuum, bool side_brush, DoneCallback cb) {
  if (!this->active_ || this->disabling_) {
    if (cb)
      cb(false);
    return false;
  }
  // Shared completion counter across the (up to three) motor commands.
  struct Pending {
    int remaining = 0;
    bool failed = false;
    DoneCallback cb;
  };
  auto pending = std::make_shared<Pending>();
  pending->cb = cb;
  auto done = [pending](bool ok) {
    if (!ok)
      pending->failed = true;
    if (--pending->remaining <= 0 && pending->cb)
      pending->cb(!pending->failed);
  };
  // Count first so a synchronous failure cannot complete early.
  pending->remaining = (brush != this->brush_on_) + (vacuum != this->vacuum_on_) + (side_brush != this->side_brush_on_);
  if (pending->remaining == 0) {
    if (cb)
      cb(true);
    return true;
  }
  if (brush != this->brush_on_) {
    this->brush_on_ = brush;
    this->client_->set_motor_brush(brush ? this->brush_rpm_ : 0, done);
  }
  if (vacuum != this->vacuum_on_) {
    this->vacuum_on_ = vacuum;
    this->client_->set_motor_vacuum(vacuum, this->vacuum_pct_, done);
  }
  if (side_brush != this->side_brush_on_) {
    this->side_brush_on_ = side_brush;
    this->client_->set_motor_side_brush(side_brush, this->side_brush_mw_, done);
  }
  this->changed_();
  return true;
}

void ManualController::tick(uint32_t now) {
  // Recover from a stuck enable (e.g. the queue was full).
  if (this->enabling_ && now - this->enabling_since_ >= MANUAL_ENABLE_TIMEOUT_MS) {
    this->enabling_ = false;
    this->changed_();
  }
  if (!this->active_ || this->disabling_)
    return;
  bool queue_ok = this->client_->queue().depth() <= QUEUE_MAX_SIZE / 2;
  if (queue_ok && !this->safety_poll_in_flight_ && now - this->last_safety_poll_ >= MANUAL_SAFETY_POLL_MS) {
    this->last_safety_poll_ = now;
    this->poll_bumpers_();
  }
  if (queue_ok && this->wheels_moving_ && !this->stall_poll_in_flight_ &&
      now - this->last_stall_poll_ >= MANUAL_STALL_POLL_MS) {
    this->last_stall_poll_ = now;
    this->poll_stall_();
  }
  // Watchdog: stop the wheels when the client stopped sending moves.
  if (this->wheels_moving_ && now - this->last_move_ms_ >= MANUAL_MOVE_TIMEOUT_MS)
    this->stop_wheels_();
}

void ManualController::poll_bumpers_() {
  this->safety_poll_in_flight_ = true;
  this->client_->get_digital_sensors(
      [this](bool ok, const DigitalSensorData &d) {
        this->safety_poll_in_flight_ = false;
        if (!ok || !this->active_)
          return;
        SafetyState prev = this->safety_;
        this->safety_.bumper_front_left = d.l_front || d.l_lds;
        this->safety_.bumper_front_right = d.r_front || d.r_lds;
        this->safety_.bumper_side_left = d.l_side;
        this->safety_.bumper_side_right = d.r_side;
        this->safety_.wheel_lifted = d.left_wheel_extended || d.right_wheel_extended;
        if (this->safety_.wheel_lifted && !prev.wheel_lifted) {
          this->stop_wheels_();
          this->stop_all_motors_();
        }
        if ((this->safety_.bumper_front_left && !prev.bumper_front_left) ||
            (this->safety_.bumper_front_right && !prev.bumper_front_right) ||
            (this->safety_.bumper_side_left && !prev.bumper_side_left) ||
            (this->safety_.bumper_side_right && !prev.bumper_side_right))
          this->stop_wheels_();
        this->changed_();
      },
      PRIORITY_HIGH);
}

void ManualController::poll_stall_() {
  this->stall_poll_in_flight_ = true;
  this->client_->get_motors(
      [this](bool ok, const MotorData &m) {
        this->stall_poll_in_flight_ = false;
        if (!ok || !this->active_ || !this->wheels_moving_)
          return;
        if (stall_step(this->safety_, this->stall_count_, this->last_left_mm_, this->last_right_mm_, m.left_wheel_load,
                       m.right_wheel_load, this->stall_pct_)) {
          this->stop_wheels_();
          this->changed_();
        }
      },
      PRIORITY_HIGH);
}

void ManualController::stop_wheels_() {
  this->wheels_moving_ = false;
  this->client_->set_motor_wheels(0, 0, 0);
}

void ManualController::stop_all_motors_() {
  if (this->brush_on_) {
    this->client_->set_motor_brush(0);
    this->brush_on_ = false;
  }
  if (this->vacuum_on_) {
    this->client_->set_motor_vacuum(false, 0);
    this->vacuum_on_ = false;
  }
  if (this->side_brush_on_) {
    this->client_->set_motor_side_brush(false, 0);
    this->side_brush_on_ = false;
  }
}

}  // namespace neatocore
