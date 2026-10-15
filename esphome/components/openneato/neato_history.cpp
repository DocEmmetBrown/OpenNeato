// Ported from OpenNeato firmware/src/cleaning_history.cpp (MIT)
#include "neato_history.h"

#include <algorithm>
#include <cmath>

namespace neatocore {

// -- PoseRing ----------------------------------------------------------------

PoseRing::PoseRing(size_t capacity) : buf_(new HistoryEntry[capacity < 1 ? 1 : capacity]),
                                      capacity_(capacity < 1 ? 1 : capacity) {}

uint32_t PoseRing::push(HistoryEntry entry) {
  entry.seq = ++this->last_seq_;
  this->buf_[this->head_] = entry;
  this->head_ = (this->head_ + 1) % this->capacity_;
  if (this->count_ < this->capacity_)
    this->count_++;
  return entry.seq;
}

uint32_t PoseRing::first_seq() const {
  if (this->count_ == 0)
    return 0;
  return this->last_seq_ - static_cast<uint32_t>(this->count_) + 1;
}

std::vector<const HistoryEntry *> PoseRing::since(uint32_t since_seq, size_t max, bool &more) const {
  std::vector<const HistoryEntry *> out;
  more = false;
  if (this->count_ == 0 || since_seq >= this->last_seq_)
    return out;
  uint32_t first = this->first_seq();
  size_t skip = since_seq >= first ? static_cast<size_t>(since_seq - first + 1) : 0;
  size_t available = this->count_ - skip;
  size_t n = available < max ? available : max;
  more = available > n;
  size_t oldest = (this->head_ + this->capacity_ - this->count_) % this->capacity_;
  out.reserve(n);
  for (size_t i = 0; i < n; i++)
    out.push_back(&this->buf_[(oldest + skip + i) % this->capacity_]);
  return out;
}

std::string entry_json(const HistoryEntry &e) {
  std::string out = "{\"seq\":" + std::to_string(e.seq) + ",\"sid\":" + std::to_string(e.sid);
  if (e.type == EntryType::RECHARGE) {
    out += ",\"type\":\"recharge\",\"x\":" + format_float(e.x, 3) + ",\"y\":" + format_float(e.y, 3) + "}";
    return out;
  }
  out += ",\"x\":" + format_float(e.x, 3) + ",\"y\":" + format_float(e.y, 3) +
         ",\"t\":" + format_float(static_cast<float>(e.theta_ddeg) / 10.0f, 1) + ",\"ts\":" + format_float(e.ts, 1);
  // `b` is omitted when the motor read failed ("unknown" vs "stopped").
  if (e.brush_rpm >= 0)
    out += ",\"b\":" + std::to_string(e.brush_rpm);
  out += "}";
  return out;
}

// -- SessionRecorder ---------------------------------------------------------

bool SessionRecorder::on_state(const std::string &ui, const std::string &robot_state, int battery,
                               int64_t now_epoch, uint32_t now_ms) {
  bool is_docking = is_docking_state(ui);
  bool is_cleaning = is_cleaning_state(ui);
  bool is_suspended = is_suspended_state(ui);
  bool charging_mid_clean = is_charging_mid_clean(robot_state);

  if (!this->collecting_) {
    bool was_cleaning = is_cleaning_state(this->prev_ui_);
    // CLEANINGSUSPENDED + Charging_Cleaning = docked mid-clean, will resume.
    bool mid_clean_recharge = is_suspended && charging_mid_clean;
    this->prev_ui_ = ui;
    if (was_cleaning || !(is_cleaning || mid_clean_recharge))
      return false;
    this->start_(ui, battery, now_epoch, now_ms);
  } else {
    this->prev_ui_ = ui;
  }

  // Mid-clean recharge (DOCKING on the way back or CLEANINGSUSPENDED on the dock).
  if ((is_docking || is_suspended) && charging_mid_clean) {
    if (!this->recharging_) {
      this->recharging_ = true;
      this->recharge_count_++;
      if (this->has_prev_pose_) {
        HistoryEntry e{};
        e.sid = this->sid_;
        e.x = this->prev_x_;
        e.y = this->prev_y_;
        e.brush_rpm = -1;
        e.type = EntryType::RECHARGE;
        this->ring_.push(e);
      }
    }
    return false;
  }
  if (this->recharging_ && is_cleaning)
    this->recharging_ = false;

  if (!is_cleaning && !is_docking && !is_suspended) {
    this->stop_(battery, now_epoch, now_ms);
    return false;
  }
  this->paused_ = is_paused_state(ui);
  return !this->paused_;
}

bool SessionRecorder::on_error(bool has_error) {
  if (!this->collecting_)
    return false;
  if (has_error && !this->prev_had_error_)
    this->errors_during_clean_++;
  this->prev_had_error_ = has_error;
  return !this->recharging_;
}

void SessionRecorder::on_pose(const Pose &pose, int brush_rpm) {
  if (!this->collecting_ || this->recharging_ || this->paused_)
    return;
  // Localization resets to origin right before a session ends: drop that pose
  // if the robot was far from origin (a genuine return passes gradually).
  if (this->has_prev_pose_ && std::fabs(pose.x) < 0.001f && std::fabs(pose.y) < 0.001f &&
      std::fabs(pose.theta) < 0.1f) {
    float prev_dist = std::sqrt(this->prev_x_ * this->prev_x_ + this->prev_y_ * this->prev_y_);
    if (prev_dist > 1.0f)
      return;
  }
  this->update_accumulators_(pose.x, pose.y, pose.theta);
  HistoryEntry e{};
  e.sid = this->sid_;
  e.x = pose.x;
  e.y = pose.y;
  e.ts = pose.time;
  float t = std::round(pose.theta * 10.0f);
  e.theta_ddeg = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, t)));
  e.brush_rpm = static_cast<int16_t>(brush_rpm < 0 ? -1 : std::min(brush_rpm, 32767));
  e.type = EntryType::POSE;
  this->ring_.push(e);
  this->snapshot_count_++;
}

void SessionRecorder::start_(const std::string &ui, int battery, int64_t now_epoch, uint32_t now_ms) {
  this->collecting_ = true;
  this->has_session_ = true;
  this->recharging_ = false;
  this->paused_ = false;
  this->mode_ = clean_mode_from_state(ui);
  this->start_epoch_ = now_epoch > 0 ? now_epoch : 0;
  this->sid_ = static_cast<uint32_t>(this->start_epoch_);
  this->start_ms_ = now_ms;
  this->end_duration_s_ = 0;
  this->battery_start_ = battery;
  this->battery_end_ = -1;
  this->snapshot_count_ = 0;
  this->recharge_count_ = 0;
  this->total_distance_ = 0;
  this->total_rotation_ = 0;
  this->max_dist_from_origin_ = 0;
  this->errors_during_clean_ = 0;
  this->prev_had_error_ = false;
  this->has_prev_pose_ = false;
  this->prev_x_ = this->prev_y_ = this->prev_theta_ = 0;
  this->origin_x_ = this->origin_y_ = 0;
  this->cells_.clear();
  this->cells_.shrink_to_fit();
}

void SessionRecorder::stop_(int battery, int64_t now_epoch, uint32_t now_ms) {
  this->end_duration_s_ = this->duration_s(now_epoch, now_ms);
  this->collecting_ = false;
  this->recharging_ = false;
  this->paused_ = false;
  this->battery_end_ = battery;
  // Too short to render a useful map: discard (previous is left untouched).
  if (this->snapshot_count_ < HISTORY_MIN_SNAPSHOTS)
    return;

  std::string s = "{\"type\":\"summary\"";
  if (now_epoch > 0)
    s += ",\"time\":" + std::to_string(now_epoch);
  s += ",\"duration\":" + std::to_string(this->end_duration_s_);
  s += ",\"mode\":\"" + this->mode_ + "\"";
  s += ",\"recharges\":" + std::to_string(this->recharge_count_);
  s += ",\"snapshots\":" + std::to_string(this->snapshot_count_);
  s += ",\"distanceTraveled\":" + format_float(this->total_distance_, 2);
  s += ",\"maxDistFromOrigin\":" + format_float(this->max_dist_from_origin_, 2);
  s += ",\"totalRotation\":" + format_float(this->total_rotation_, 1);
  s += ",\"areaCovered\":" + format_float(this->area_m2(), 2);
  s += ",\"errorsDuringClean\":" + std::to_string(this->errors_during_clean_);
  if (this->battery_start_ >= 0)
    s += ",\"batteryStart\":" + std::to_string(this->battery_start_);
  if (battery >= 0)
    s += ",\"batteryEnd\":" + std::to_string(battery);
  s += "}";

  this->has_previous_ = true;
  this->previous_id_ = this->sid_;
  this->previous_header_ = this->header_json_();
  this->previous_summary_ = s;
  this->cells_.clear();
  this->cells_.shrink_to_fit();
}

uint32_t SessionRecorder::duration_s(int64_t now_epoch, uint32_t now_ms) const {
  if (!this->has_session_)
    return 0;
  if (!this->collecting_)
    return this->end_duration_s_;
  if (this->start_epoch_ > 0 && now_epoch > this->start_epoch_)
    return static_cast<uint32_t>(now_epoch - this->start_epoch_);
  return (now_ms - this->start_ms_) / 1000;
}

void SessionRecorder::update_accumulators_(float x, float y, float theta) {
  if (!this->has_prev_pose_) {
    this->origin_x_ = x;
    this->origin_y_ = y;
    this->has_prev_pose_ = true;
  } else {
    float dx = x - this->prev_x_;
    float dy = y - this->prev_y_;
    this->total_distance_ += std::sqrt(dx * dx + dy * dy);
    float d_theta = theta - this->prev_theta_;
    if (d_theta > 180.0f)
      d_theta -= 360.0f;
    if (d_theta < -180.0f)
      d_theta += 360.0f;
    this->total_rotation_ += std::fabs(d_theta);
  }
  this->prev_x_ = x;
  this->prev_y_ = y;
  this->prev_theta_ = theta;

  float dox = x - this->origin_x_;
  float doy = y - this->origin_y_;
  float dist = std::sqrt(dox * dox + doy * doy);
  if (dist > this->max_dist_from_origin_)
    this->max_dist_from_origin_ = dist;

  int ix = static_cast<int>(std::floor(x / HISTORY_AREA_CELL_M));
  int iy = static_cast<int>(std::floor(y / HISTORY_AREA_CELL_M));
  uint32_t key = (static_cast<uint32_t>(ix & 0xFFFF) << 16) | static_cast<uint32_t>(iy & 0xFFFF);
  auto it = std::lower_bound(this->cells_.begin(), this->cells_.end(), key);
  if ((it == this->cells_.end() || *it != key) && this->cells_.size() < HISTORY_MAX_CELLS)
    this->cells_.insert(it, key);
}

std::string SessionRecorder::header_json_() const {
  std::string h = "{\"type\":\"session\",\"mode\":\"" + this->mode_ + "\"";
  if (this->start_epoch_ > 0)
    h += ",\"time\":" + std::to_string(this->start_epoch_);
  if (this->battery_start_ >= 0)
    h += ",\"battery\":" + std::to_string(this->battery_start_);
  h += "}";
  return h;
}

std::string SessionRecorder::current_json() const {
  if (!this->collecting_)
    return "null";
  return "{\"id\":" + std::to_string(this->sid_) + ",\"active\":" + (this->active() ? "true" : "false") +
         ",\"header\":" + this->header_json_() + "}";
}

std::string SessionRecorder::previous_json() const {
  if (!this->has_previous_)
    return "null";
  return "{\"id\":" + std::to_string(this->previous_id_) + ",\"header\":" + this->previous_header_ +
         ",\"summary\":" + this->previous_summary_ + "}";
}

std::string SessionRecorder::entries_json(uint32_t since_seq, bool &more) const {
  auto entries = this->ring_.since(since_seq, POSE_BATCH_MAX, more);
  std::string out;
  out.reserve(2 + entries.size() * 96);
  out += '[';
  for (size_t i = 0; i < entries.size(); i++) {
    if (i > 0)
      out += ',';
    out += entry_json(*entries[i]);
  }
  out += ']';
  return out;
}

std::string SessionRecorder::batch_json(uint32_t since_seq, uint32_t boot_id) const {
  bool more = false;
  std::string entries = this->entries_json(since_seq, more);
  return "{\"boot\":" + std::to_string(boot_id) + ",\"seq\":" + std::to_string(this->ring_.last_seq()) +
         ",\"first_seq\":" + std::to_string(this->ring_.first_seq()) + ",\"more\":" + (more ? "true" : "false") +
         ",\"current\":" + this->current_json() + ",\"previous\":" + this->previous_json() + ",\"entries\":" + entries +
         "}";
}

}  // namespace neatocore
