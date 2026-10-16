// Mapping run: the robot's exploration run (Clean Explore), with a fallback.
//
// Exploration is the Neato app's persistent-map feature: before driving off
// the robot asks the (now shut down) Neato cloud for a map slot, and without
// one it raises a persistent-map alert (234 "Failed to load persistent map"
// and friends) and stays put. MappingRun watches for that and tells the
// component to fall back to a quiet house clean (Eco, Quick navigation),
// then to restore the user's settings once that clean is over.
// Pure C++17, host-tested.
#ifndef OPENNEATO_NEATO_MAPPING_H
#define OPENNEATO_NEATO_MAPPING_H

#include <cstdint>
#include <string>

namespace neatocore {

class MappingRun {
 public:
  // STOP: cancel the refused exploration (app stop event) and clear the alert.
  // START: Eco on and a house clean (navigation forced to Quick meanwhile).
  // RESTORE: put Eco mode back.
  enum class Action : uint8_t { NONE, STOP, START, RESTORE };

  // Exploration requested now.
  void start(uint32_t now_ms);
  // One GetState/GetErr snapshot; returns what the component must do.
  Action update(const std::string &ui, bool has_error, int error_code, uint32_t now_ms);
  // True while the fallback clean is starting or running (settings overridden).
  bool overriding() const { return this->phase_ == Phase::STARTING || this->phase_ == Phase::RUNNING; }
  bool busy() const { return this->phase_ != Phase::IDLE; }

  // Alerts meaning the persistent map could not be had. 236 (acquiring map
  // data) is the attempt in progress, not a failure.
  static bool is_persistent_map_failure(int error_code);

  static constexpr uint32_t EXPLORE_TIMEOUT_MS = 120000;
  // Re-send the stop / start when the robot has not reacted after this long,
  // and give up after the matching timeout.
  static constexpr uint32_t RETRY_MS = 30000;
  static constexpr uint32_t STOP_TIMEOUT_MS = 120000;
  static constexpr uint32_t START_TIMEOUT_MS = 120000;

 private:
  enum class Phase : uint8_t { IDLE, EXPLORING, STOPPING, STARTING, RUNNING };
  Action enter_(Phase phase, uint32_t now_ms, Action action);
  Phase phase_{Phase::IDLE};
  uint32_t since_ms_{0};
  uint32_t sent_ms_{0};
};

}  // namespace neatocore

#endif  // OPENNEATO_NEATO_MAPPING_H
