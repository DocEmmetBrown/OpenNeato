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
  enum class Action : uint8_t { NONE, FALLBACK, RESTORE };

  // Exploration requested now.
  void start(uint32_t now_ms);
  // One GetState/GetErr snapshot; returns what the component must do.
  Action update(const std::string &ui, bool has_error, int error_code, uint32_t now_ms);
  // True from a fallback until its clean is over (settings overridden meanwhile).
  bool overriding() const { return this->phase_ == Phase::FALLBACK_STARTING || this->phase_ == Phase::FALLBACK_RUNNING; }
  bool busy() const { return this->phase_ != Phase::IDLE; }

  static bool is_persistent_map_alert(int error_code);

  // Time allowed for the exploration to start or fail, and for the fallback
  // clean to start.
  static constexpr uint32_t EXPLORE_TIMEOUT_MS = 90000;
  static constexpr uint32_t FALLBACK_START_TIMEOUT_MS = 120000;

 private:
  enum class Phase : uint8_t { IDLE, EXPLORING, FALLBACK_STARTING, FALLBACK_RUNNING };
  Phase phase_{Phase::IDLE};
  uint32_t since_ms_{0};
};

}  // namespace neatocore

#endif  // OPENNEATO_NEATO_MAPPING_H
