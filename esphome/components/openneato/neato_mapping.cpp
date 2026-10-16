#include "neato_mapping.h"

namespace neatocore {

namespace {
bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }
bool running(const std::string &ui) {
  return contains(ui, "CLEANINGRUNNING") || contains(ui, "DOCKING") || contains(ui, "CLEANINGPAUSED") ||
         contains(ui, "CLEANINGSUSPENDED") || contains(ui, "EXPLOR");
}
bool idle(const std::string &ui) { return ui == "UIMGR_STATE_IDLE" || ui == "UIMGR_STATE_STANDBY"; }
}  // namespace

bool StartGuard::is_persistent_map_failure(int error_code) {
  switch (error_code) {
    case 231:  // UI_ALERT_PERSISTENT_RELOCALIZATION_FAIL
    case 234:  // UI_ALERT_PM_LOAD_FAIL
    case 235:  // UI_ALERT_PM_SETUP_FAIL
    case 238:  // UI_ALERT_PM_START_CLEAN_FAIL
    case 239:  // UI_ALERT_NAV_FLOORPLAN_NOT_CREATED
      return true;
    default:
      return false;
  }
}

void StartGuard::begin(Kind kind, uint32_t now_ms) {
  this->kind_ = kind;
  this->retried_ = false;
  this->giving_up_ = false;
  this->gave_up_ = false;
  this->enter_(Phase::STARTING, now_ms, Action::NONE);
}

StartGuard::Action StartGuard::enter_(Phase phase, uint32_t now_ms, Action action) {
  this->phase_ = phase;
  this->since_ms_ = now_ms;
  this->sent_ms_ = now_ms;
  this->saw_disconnect_ = false;
  return action;
}

StartGuard::Action StartGuard::finish_(bool gave_up) {
  this->phase_ = Phase::IDLE;
  this->gave_up_ = gave_up;
  return Action::DONE;
}

StartGuard::Action StartGuard::update(const std::string &ui, bool has_error, int error_code, bool connected,
                                      uint32_t now_ms) {
  uint32_t elapsed = now_ms - this->since_ms_;
  bool retry_due = now_ms - this->sent_ms_ >= RETRY_MS;
  switch (this->phase_) {
    case Phase::IDLE:
      return Action::NONE;

    case Phase::STARTING:
      if (has_error && is_persistent_map_failure(error_code)) {
        this->giving_up_ = this->retried_;  // refused again after a restart: stop and give up
        return this->enter_(Phase::STOPPING, now_ms, Action::STOP);
      }
      if (running(ui)) {
        if (this->kind_ != Kind::MAPPING)
          return this->finish_(false);  // nothing more to watch
        this->phase_ = Phase::RUNNING;
        return Action::NONE;
      }
      if (elapsed > START_TIMEOUT_MS)
        return this->finish_(true);
      if (retry_due && idle(ui)) {  // the start was not taken
        this->sent_ms_ = now_ms;
        return Action::START;
      }
      return Action::NONE;

    case Phase::STOPPING:
      if (idle(ui)) {
        if (this->giving_up_)
          return this->finish_(true);
        return this->enter_(Phase::RESTARTING, now_ms, Action::RESTART);
      }
      if (elapsed > STOP_TIMEOUT_MS)
        return this->finish_(true);
      if (retry_due) {
        this->sent_ms_ = now_ms;
        return Action::STOP;
      }
      return Action::NONE;

    case Phase::RESTARTING:
      if (!connected)
        this->saw_disconnect_ = true;
      if (connected && idle(ui) && elapsed >= RESTART_MIN_MS &&
          (this->saw_disconnect_ || elapsed >= RESTART_MIN_MS * 3)) {
        this->retried_ = true;
        return this->enter_(Phase::STARTING, now_ms, Action::START);
      }
      if (elapsed > RESTART_TIMEOUT_MS)
        return this->finish_(true);
      return Action::NONE;

    case Phase::RUNNING:
      if (idle(ui))
        return this->finish_(false);
      return Action::NONE;
  }
  return Action::NONE;
}

}  // namespace neatocore
