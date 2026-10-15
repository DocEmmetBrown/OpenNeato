// Ported from OpenNeato firmware/src/notification_manager.cpp (MIT)
//
// Turns GetState/GetErr snapshots into `robot_event` event types. Same rules as
// the integration's RobotEventTracker (custom_components/openneato/event.py)
// and docs/esphome-contract.md. Pure C++17.
#pragma once

#include <string>
#include <vector>

namespace neatocore {

#define NEATO_EVENT_CLEANING_STARTED "cleaning_started"
#define NEATO_EVENT_CLEANING_DONE "cleaning_done"
#define NEATO_EVENT_DOCKING_RECHARGE "docking_recharge"
#define NEATO_EVENT_RETURNED_TO_BASE "returned_to_base"
#define NEATO_EVENT_ERROR "error"
#define NEATO_EVENT_ALERT "alert"

class EventTracker {
 public:
  // Feed a GetState result (raw UI state, robot state). Returns the events it causes.
  std::vector<const char *> update_state(const std::string &ui, const std::string &robot_state);
  // Feed a GetErr result. The first snapshot only seeds the tracker, so a
  // condition already present at boot is not announced.
  std::vector<const char *> update_error(bool has_error, int code, const std::string &kind);

  bool cleaning_before_dock() const { return this->cleaning_before_dock_; }

 protected:
  std::string prev_ui_;
  bool cleaning_before_dock_{false};
  bool prev_has_error_{false};
  int prev_error_code_{0};
  bool error_initialized_{false};
};

}  // namespace neatocore
