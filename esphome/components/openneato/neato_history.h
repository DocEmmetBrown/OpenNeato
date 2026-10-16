// Ported from OpenNeato firmware/src/cleaning_history.{h,cpp} (MIT)
//
// Cleaning session recorder for the ESPHome firmware: same session rules as the
// legacy CleaningHistory (start on house/spot cleaning, pause collection while
// paused or recharging, end when idle, drop the origin-reset pose, recharge
// markers, summary accumulators), but instead of JSONL files on SPIFFS the
// poses go into a RAM ring buffer read by the `get_pose_batch` action.
// Pure C++17.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "neato_protocol.h"

namespace neatocore {

static const uint32_t HISTORY_POSE_INTERVAL_MS = 2000;
static const float HISTORY_AREA_CELL_M = 0.5f;
static const size_t HISTORY_MAX_CELLS = 4096;
static const size_t HISTORY_MIN_SNAPSHOTS = 3;
static const size_t POSE_BATCH_MAX = 100;

enum class EntryType : uint8_t { POSE = 0, RECHARGE = 1 };

struct HistoryEntry {
  uint32_t seq;
  uint32_t sid;
  float x;
  float y;
  float ts;
  int16_t theta_ddeg;  // tenths of a degree
  int16_t brush_rpm;   // -1 = unknown (motor read failed)
  EntryType type;
};

// Fixed-capacity ring of entries; seq is monotonically increasing, never reset.
class PoseRing {
 public:
  explicit PoseRing(size_t capacity);
  uint32_t push(HistoryEntry entry);  // assigns and returns the seq
  size_t size() const { return this->count_; }
  size_t capacity() const { return this->capacity_; }
  uint32_t last_seq() const { return this->last_seq_; }
  uint32_t first_seq() const;  // oldest seq still buffered, 0 when empty
  // Up to `max` entries with seq > since (oldest first); `more` = others remain.
  std::vector<const HistoryEntry *> since(uint32_t since_seq, size_t max, bool &more) const;

 protected:
  std::unique_ptr<HistoryEntry[]> buf_;
  size_t capacity_;
  size_t head_{0};  // next write index
  size_t count_{0};
  uint32_t last_seq_{0};
};

std::string entry_json(const HistoryEntry &e);

class SessionRecorder {
 public:
  explicit SessionRecorder(size_t ring_capacity) : ring_(ring_capacity) {}

  // Feed each GetState result. `battery` = latest FuelPercent (-1 unknown),
  // `now_epoch` = UTC seconds (0 = clock not valid). Returns true when the
  // caller should continue the cycle with GetErr -> on_error().
  // The next session starting within 5 min gets this header mode (e.g.
  // "explore" for a mapping run, which reports like a house clean).
  void expect_mode(const std::string &mode, uint32_t now_ms) {
    this->pending_mode_ = mode;
    this->pending_mode_ms_ = now_ms;
  }
  bool on_state(const std::string &ui, const std::string &robot_state, int battery, int64_t now_epoch,
                uint32_t now_ms);
  // Returns true when the caller should fetch a pose (GetRobotPos Smooth +
  // GetMotors) and call on_pose().
  bool on_error(bool has_error);
  // brush_rpm < 0 = motor read failed.
  void on_pose(const Pose &pose, int brush_rpm);

  bool collecting() const { return this->collecting_; }
  bool active() const { return this->collecting_ && !this->recharging_ && !this->paused_; }
  const PoseRing &ring() const { return this->ring_; }

  // Stats of the current (or last) session for the `Clean ...` sensors.
  bool has_session() const { return this->has_session_; }
  int64_t start_epoch() const { return this->start_epoch_; }
  uint32_t duration_s(int64_t now_epoch, uint32_t now_ms) const;
  float area_m2() const { return static_cast<float>(this->cells_.size()) * HISTORY_AREA_CELL_M * HISTORY_AREA_CELL_M; }
  float distance_m() const { return this->total_distance_; }
  int recharges() const { return this->recharge_count_; }
  int battery_start() const { return this->battery_start_; }
  int battery_end() const { return this->battery_end_; }
  size_t snapshots() const { return this->snapshot_count_; }

  // JSON pieces of the get_pose_batch response.
  std::string current_json() const;   // object or "null"
  std::string previous_json() const;  // object or "null"
  std::string entries_json(uint32_t since_seq, bool &more) const;
  std::string batch_json(uint32_t since_seq, uint32_t boot_id) const;

 protected:
  void start_(const std::string &ui, int battery, int64_t now_epoch, uint32_t now_ms);
  void stop_(int battery, int64_t now_epoch, uint32_t now_ms);
  void update_accumulators_(float x, float y, float theta);
  std::string header_json_() const;

  PoseRing ring_;
  std::string prev_ui_;
  bool collecting_{false};
  bool recharging_{false};
  bool paused_{false};
  bool has_session_{false};

  uint32_t sid_{0};
  std::string mode_;
  std::string pending_mode_;
  uint32_t pending_mode_ms_{0};
  int64_t start_epoch_{0};
  uint32_t start_ms_{0};
  uint32_t end_duration_s_{0};
  int battery_start_{-1};
  int battery_end_{-1};
  size_t snapshot_count_{0};
  int recharge_count_{0};
  float total_distance_{0};
  float total_rotation_{0};
  float max_dist_from_origin_{0};
  int errors_during_clean_{0};
  bool prev_had_error_{false};
  float prev_x_{0}, prev_y_{0}, prev_theta_{0};
  float origin_x_{0}, origin_y_{0};
  bool has_prev_pose_{false};
  std::vector<uint32_t> cells_;  // sorted visited 0.5 m grid cells

  // Last completed session.
  bool has_previous_{false};
  uint32_t previous_id_{0};
  std::string previous_header_;
  std::string previous_summary_;
};

}  // namespace neatocore
