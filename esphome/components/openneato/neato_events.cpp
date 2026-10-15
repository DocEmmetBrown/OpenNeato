// Ported from OpenNeato firmware/src/notification_manager.cpp (MIT)
#include "neato_events.h"

#include "neato_protocol.h"

namespace neatocore {

std::vector<const char *> EventTracker::update_state(const std::string &ui, const std::string &robot_state) {
  std::vector<const char *> events;
  if (ui.empty())
    return events;
  if (this->prev_ui_.empty()) {
    this->prev_ui_ = ui;
    return events;
  }
  const std::string &prev = this->prev_ui_;
  bool was_cleaning = contains(prev, "CLEANINGRUNNING");
  bool was_docking = contains(prev, "DOCKING");
  bool is_cleaning = contains(ui, "CLEANINGRUNNING");
  bool is_docking = contains(ui, "DOCKING");
  bool is_suspended = contains(ui, "CLEANINGSUSPENDED");
  bool is_idle = is_idle_state(ui);
  bool is_recharging = is_charging_mid_clean(robot_state);

  if (was_cleaning && is_docking)
    this->cleaning_before_dock_ = true;

  bool prev_in_clean = contains(prev, "CLEANING") || contains(prev, "DOCKING");
  if (is_cleaning && !prev_in_clean)
    events.push_back(NEATO_EVENT_CLEANING_STARTED);

  if (is_docking && !was_docking && is_recharging)
    events.push_back(NEATO_EVENT_DOCKING_RECHARGE);

  bool docking_done = was_docking && this->cleaning_before_dock_ && !is_recharging;
  bool suspended_done = contains(prev, "CLEANINGSUSPENDED") && this->cleaning_before_dock_;
  if ((was_cleaning || docking_done || suspended_done) && is_idle)
    events.push_back(NEATO_EVENT_CLEANING_DONE);
  if (was_docking && is_idle && !is_recharging)
    events.push_back(NEATO_EVENT_RETURNED_TO_BASE);

  // Preserved through DOCKING -> CLEANINGSUSPENDED (mid-clean recharge).
  if (was_docking && !is_docking && !is_suspended)
    this->cleaning_before_dock_ = false;
  if (is_idle)
    this->cleaning_before_dock_ = false;

  this->prev_ui_ = ui;
  return events;
}

std::vector<const char *> EventTracker::update_error(bool has_error, int code, const std::string &kind) {
  std::vector<const char *> events;
  if (this->error_initialized_ && has_error && (!this->prev_has_error_ || code != this->prev_error_code_))
    events.push_back(kind == "warning" ? NEATO_EVENT_ALERT : NEATO_EVENT_ERROR);
  this->prev_has_error_ = has_error;
  this->prev_error_code_ = code;
  this->error_initialized_ = true;
  return events;
}

}  // namespace neatocore
