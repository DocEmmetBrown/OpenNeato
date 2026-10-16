#include "neato_mapping.h"

namespace neatocore {

namespace {
bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }
bool running(const std::string &ui) { return contains(ui, "CLEANINGRUNNING") || contains(ui, "EXPLOR"); }
bool idle(const std::string &ui) { return ui == "UIMGR_STATE_IDLE" || ui == "UIMGR_STATE_STANDBY"; }
}  // namespace

bool MappingRun::is_persistent_map_alert(int error_code) {
  switch (error_code) {
    case 231:  // UI_ALERT_PERSISTENT_RELOCALIZATION_FAIL
    case 234:  // UI_ALERT_PM_LOAD_FAIL
    case 235:  // UI_ALERT_PM_SETUP_FAIL
    case 236:  // UI_ALERT_ACQUIRING_PERSISTENT_MAP_IDS
    case 238:  // UI_ALERT_PM_START_CLEAN_FAIL
    case 239:  // UI_ALERT_NAV_FLOORPLAN_NOT_CREATED
      return true;
    default:
      return false;
  }
}

void MappingRun::start(uint32_t now_ms) {
  this->phase_ = Phase::EXPLORING;
  this->since_ms_ = now_ms;
}

MappingRun::Action MappingRun::update(const std::string &ui, bool has_error, int error_code, uint32_t now_ms) {
  uint32_t elapsed = now_ms - this->since_ms_;
  switch (this->phase_) {
    case Phase::IDLE:
      return Action::NONE;
    case Phase::EXPLORING:
      if (has_error && is_persistent_map_alert(error_code)) {
        this->phase_ = Phase::FALLBACK_STARTING;
        this->since_ms_ = now_ms;
        return Action::FALLBACK;
      }
      // Exploring for real (the robot still had a usable map), or nothing
      // happened at all: either way there is nothing left to do.
      if (running(ui) || elapsed > EXPLORE_TIMEOUT_MS)
        this->phase_ = Phase::IDLE;
      return Action::NONE;
    case Phase::FALLBACK_STARTING:
      if (running(ui)) {
        this->phase_ = Phase::FALLBACK_RUNNING;
        return Action::NONE;
      }
      if (elapsed > FALLBACK_START_TIMEOUT_MS) {
        this->phase_ = Phase::IDLE;
        return Action::RESTORE;
      }
      return Action::NONE;
    case Phase::FALLBACK_RUNNING:
      if (idle(ui)) {
        this->phase_ = Phase::IDLE;
        return Action::RESTORE;
      }
      return Action::NONE;
  }
  return Action::NONE;
}

}  // namespace neatocore
