// Ported from OpenNeato firmware/src/neato_serial.{h,cpp} (MIT)
//
// Typed robot API on top of SerialQueue: cached/coalesced sensor queries, the
// SKey bootstrap (GetVersion with exponential backoff) and the action commands
// (SetEvent cleaning control, motors, user settings, power, ...).
// Pure C++17: no ESPHome dependency.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "neato_async_cache.h"
#include "neato_protocol.h"
#include "neato_serial_queue.h"

namespace neatocore {

static const uint32_t SKEY_RETRY_INITIAL_MS = 2000;
static const uint32_t SKEY_RETRY_MAX_MS = 30000;

static const uint32_t CACHE_TTL_STATE = 1500;
static const uint32_t CACHE_TTL_CHARGER = 5000;
static const uint32_t CACHE_TTL_SENSORS = 400;
static const uint32_t CACHE_TTL_SLOW = 60000;

using DoneCallback = std::function<void(bool)>;

class NeatoClient {
 public:
  NeatoClient(Transport *transport, ClockFn clock);

  // Drives the queue and the SKey bootstrap. Call every loop iteration.
  void tick();

  SerialQueue &queue() { return this->queue_; }
  bool has_skey() const { return !this->skey_.empty(); }
  const std::string &skey() const { return this->skey_; }
  bool is_identified() const { return !this->identifying_; }
  // Fires once per successful identification (GetVersion with serial number).
  void on_identified(std::function<void(const VersionData &)> cb) { this->identified_cb_ = std::move(cb); }

  // -- Cached queries --------------------------------------------------------
  void get_version(std::function<void(bool, const VersionData &)> cb) { this->version_cache_.get(std::move(cb)); }
  void get_charger(std::function<void(bool, const ChargerData &)> cb) { this->charger_cache_.get(std::move(cb)); }
  void get_analog(std::function<void(bool, const AnalogData &)> cb) { this->analog_cache_.get(std::move(cb)); }
  void get_warranty(std::function<void(bool, const WarrantyData &)> cb) { this->warranty_cache_.get(std::move(cb)); }
  void get_user_settings(std::function<void(bool, const UserSettingsData &)> cb) {
    this->user_settings_cache_.get(std::move(cb));
  }
  void get_state(std::function<void(bool, const RobotState &)> cb);
  void get_error(std::function<void(bool, const ErrorData &)> cb) { this->error_cache_.get(std::move(cb)); }
  void get_pose(std::function<void(bool, const Pose &)> cb) { this->pose_cache_.get(std::move(cb)); }
  // NORMAL priority goes through the cache; higher priorities bypass it.
  void get_digital_sensors(std::function<void(bool, const DigitalSensorData &)> cb, Priority p = PRIORITY_NORMAL);
  void get_motors(std::function<void(bool, const MotorData &)> cb, Priority p = PRIORITY_NORMAL);
  // Uncached: the scan is big and only fetched on demand.
  void get_lds_scan(std::function<void(bool, std::unique_ptr<LdsScan>)> cb);

  const RobotState *cached_state() const {
    return this->state_cache_.has_cached() ? &this->state_cache_.cached() : nullptr;
  }

  // -- Actions (false = rejected before reaching the queue) ------------------
  // house (start/resume), spot (start/resume), pause, stop, dock.
  bool clean(const std::string &action, DoneCallback cb = nullptr);
  bool play_sound(int sound_id, DoneCallback cb = nullptr);
  bool test_mode(bool enable, DoneCallback cb = nullptr);
  bool set_lds_rotation(bool on, DoneCallback cb = nullptr);
  bool set_motor_wheels(int left_mm, int right_mm, int speed_mm_s, DoneCallback cb = nullptr);
  bool set_motor_brush(int rpm, DoneCallback cb = nullptr);
  bool set_motor_vacuum(bool on, int speed_pct, DoneCallback cb = nullptr);
  bool set_motor_side_brush(bool on, int power_mw, DoneCallback cb = nullptr);
  bool set_user_setting(const std::string &key, const std::string &value, DoneCallback cb = nullptr);
  // "restart" (PowerCycle) or "shutdown": TestMode On, then SetSystemMode (fire-and-forget).
  bool power_control(const std::string &action, DoneCallback cb = nullptr);
  bool clear_errors(DoneCallback cb = nullptr);
  bool new_battery(DoneCallback cb = nullptr);
  bool send_raw(const std::string &command, std::function<void(CmdStatus, const std::string &)> cb);

  void invalidate_state();
  void invalidate_user_settings() { this->user_settings_cache_.invalidate(); }
  void invalidate_version() { this->version_cache_.invalidate(); }

  // Patches ui_state to UIMGR_STATE_MANUALCLEANING while manual mode is on.
  void set_manual_clean_active(bool active) { this->manual_active_ = active; }
  bool manual_clean_active() const { return this->manual_active_; }

  // Fired when a new house/spot clean is started (not on resume).
  void on_clean_start(std::function<void()> cb) { this->clean_start_cb_ = std::move(cb); }
  // Navigation mode sent before each new house clean ("Normal" sends nothing).
  void set_nav_mode_getter(std::function<std::string()> getter) { this->nav_mode_getter_ = std::move(getter); }

 protected:
  bool enqueue_action_(const std::string &cmd, DoneCallback cb, Priority p = PRIORITY_NORMAL, bool wait = true);
  bool start_new_clean_(const char *event, DoneCallback cb, bool house);
  void start_identify_();

  template<typename T> using Parser = bool (*)(const std::string &, T &);
  template<typename T> void fetch_(const char *cmd, Parser<T> parser, std::function<void(bool, const T &)> cb,
                                   Priority p = PRIORITY_NORMAL) {
    this->queue_.enqueue(
        cmd,
        [parser, cb](CmdStatus status, const std::string &raw) {
          T data{};
          bool ok = status == CmdStatus::SUCCESS && parser(raw, data);
          if (cb)
            cb(ok, data);
        },
        p);
  }

  ClockFn clock_;
  SerialQueue queue_;
  std::string skey_;
  bool identifying_{true};
  bool identify_in_flight_{false};
  uint32_t identify_retry_at_{0};
  uint32_t identify_retry_delay_{SKEY_RETRY_INITIAL_MS};
  bool manual_active_{false};
  std::function<void(const VersionData &)> identified_cb_;
  std::function<void()> clean_start_cb_;
  std::function<std::string()> nav_mode_getter_;

  AsyncCache<VersionData> version_cache_;
  AsyncCache<ChargerData> charger_cache_;
  AsyncCache<AnalogData> analog_cache_;
  AsyncCache<WarrantyData> warranty_cache_;
  AsyncCache<UserSettingsData> user_settings_cache_;
  AsyncCache<RobotState> state_cache_;
  AsyncCache<ErrorData> error_cache_;
  AsyncCache<Pose> pose_cache_;
  AsyncCache<DigitalSensorData> digital_cache_;
  AsyncCache<MotorData> motor_cache_;
};

}  // namespace neatocore
