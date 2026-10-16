// Start guard: recovers cleans the robot refuses with a persistent-map alert,
// and runs the mapping run.
//
// Without the Neato cloud some robots get stuck starting a clean: alert 234
// "Failed to load persistent map", UI state STARTHOUSECLEANING, robot state
// ST_C_Standby, beeping. Once there, every clean is refused until the robot
// restarts (also documented upstream: renjfk/OpenNeato docs/user-guide.md,
// "Robot stuck starting a house clean"). The exploration run (Clean Explore)
// reliably puts it there, so the mapping run is a house clean with Quick
// navigation instead and Clean Explore is never sent.
//
// StartGuard follows one clean started by the bridge: on a refusal it has the
// component stop it (app stop event), restart the robot, and start it again,
// once. Pure C++17, host-tested.
#ifndef OPENNEATO_NEATO_MAPPING_H
#define OPENNEATO_NEATO_MAPPING_H

#include <cstdint>
#include <string>

namespace neatocore {

class StartGuard {
 public:
  enum class Kind : uint8_t { HOUSE, SPOT, MAPPING };
  // STOP: app stop event + clear errors. RESTART: power-cycle the robot.
  // START: start the clean again. DONE: the clean is over or given up.
  enum class Action : uint8_t { NONE, STOP, RESTART, START, DONE };

  // A clean of `kind` was just sent to the robot.
  void begin(Kind kind, uint32_t now_ms);
  // One GetState/GetErr snapshot (+ whether the robot answers); returns what
  // the component must do.
  Action update(const std::string &ui, bool has_error, int error_code, bool connected, uint32_t now_ms);

  bool busy() const { return this->phase_ != Phase::IDLE; }
  Kind kind() const { return this->kind_; }
  // A mapping run is in progress (navigation forced to Quick).
  bool mapping() const { return this->busy() && this->kind_ == Kind::MAPPING; }
  // The last update gave up (the robot refused the clean after a restart too).
  bool gave_up() const { return this->gave_up_; }

  // Alerts meaning the persistent map could not be had. 236 (acquiring map
  // data) is the attempt in progress, not a failure.
  static bool is_persistent_map_failure(int error_code);

  static constexpr uint32_t RETRY_MS = 30000;          // re-send stop/start not taken
  static constexpr uint32_t START_TIMEOUT_MS = 120000;  // clean never got going
  static constexpr uint32_t STOP_TIMEOUT_MS = 120000;
  static constexpr uint32_t RESTART_MIN_MS = 20000;     // let the robot actually go down
  static constexpr uint32_t RESTART_TIMEOUT_MS = 180000;

 private:
  enum class Phase : uint8_t { IDLE, STARTING, STOPPING, RESTARTING, RUNNING };
  Action enter_(Phase phase, uint32_t now_ms, Action action);
  Action finish_(bool gave_up);
  Phase phase_{Phase::IDLE};
  Kind kind_{Kind::HOUSE};
  uint32_t since_ms_{0};
  uint32_t sent_ms_{0};
  bool retried_{false};
  bool giving_up_{false};
  bool saw_disconnect_{false};
  bool gave_up_{false};
};

}  // namespace neatocore

#endif  // OPENNEATO_NEATO_MAPPING_H
