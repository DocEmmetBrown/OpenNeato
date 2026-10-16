// OpenNeato ESPHome hub: bridges a Neato Botvac (D3-D7) serial port to native
// ESPHome entities and API actions. Robot logic lives in the pure neato_*
// modules (ported from the OpenNeato Arduino firmware, MIT); this file is glue.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/datetime/time_entity.h"
#include "esphome/components/event/event.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "neato_client.h"
#include "neato_events.h"
#include "neato_history.h"
#include "neato_mapping.h"
#include "neato_manual.h"
#include "neato_schedule.h"

namespace esphome::openneato {

// Entity slots. The Python codegen refers to these enumerators by name.
enum TextSensorIdx : uint8_t {
  TEXT_UI_STATE,
  TEXT_ROBOT_STATE,
  TEXT_ACTIVITY,
  TEXT_ERROR_MESSAGE,
  TEXT_ERROR_KIND,
  TEXT_MODEL,
  TEXT_SERIAL_NUMBER,
  TEXT_ROBOT_FIRMWARE,
  TEXT_LDS_FIRMWARE,
  TEXT_MAIN_BOARD,
  TEXT_COUNT,
};

enum SensorIdx : uint8_t {
  SENSOR_BATTERY,
  SENSOR_BATTERY_VOLTAGE,
  SENSOR_BATTERY_CURRENT,
  SENSOR_BATTERY_TEMPERATURE,
  SENSOR_EXTERNAL_VOLTAGE,
  SENSOR_BATTERY_CYCLES,
  SENSOR_TOTAL_CLEANING_TIME,
  SENSOR_ERROR_CODE,
  SENSOR_BRUSH_RPM,
  SENSOR_VACUUM_RPM,
  SENSOR_LEFT_WHEEL_LOAD,
  SENSOR_RIGHT_WHEEL_LOAD,
  SENSOR_LDS_RPM,
  SENSOR_NEXT_CLEAN,
  SENSOR_CLEAN_STARTED,
  SENSOR_CLEAN_DURATION,
  SENSOR_CLEAN_AREA,
  SENSOR_CLEAN_DISTANCE,
  SENSOR_CLEAN_RECHARGES,
  SENSOR_CLEAN_BATTERY_USED,
  SENSOR_COUNT,
};

enum BinarySensorIdx : uint8_t {
  BINARY_CHARGING,
  BINARY_EXTERNAL_POWER,
  BINARY_PROBLEM,
  BINARY_DUSTBIN,
  BINARY_WHEEL_LIFTED,
  BINARY_FRONT_LEFT_BUMPER,
  BINARY_FRONT_RIGHT_BUMPER,
  BINARY_SIDE_LEFT_BUMPER,
  BINARY_SIDE_RIGHT_BUMPER,
  BINARY_BATTERY_OVERHEATING,
  BINARY_BATTERY_FAILURE,
  BINARY_ROBOT_CONNECTED,
  BINARY_STALL_FRONT,
  BINARY_STALL_REAR,
  BINARY_COUNT,
};

// Robot user settings switches first (index = USER_SETTING_KEYS index).
enum SwitchIdx : uint8_t {
  SWITCH_ECO_MODE,
  SWITCH_INTENSE_CLEAN,
  SWITCH_BIN_FULL_DETECTION,
  SWITCH_WALL_FOLLOWING,
  SWITCH_BUTTON_CLICK_SOUND,
  SWITCH_MELODIES,
  SWITCH_WARNING_SOUNDS,
  SWITCH_STEALTH_LED,
  SWITCH_SCHEDULE,
  SWITCH_SKIP_NEXT_CLEAN,
  SWITCH_RESTART_BEFORE_CLEAN,
  SWITCH_MANUAL_MODE,
  SWITCH_MANUAL_BRUSH,
  SWITCH_MANUAL_VACUUM,
  SWITCH_MANUAL_SIDE_BRUSH,
  SWITCH_SLOT_FIRST,  // schedule slot switches: SWITCH_SLOT_FIRST + day * 2 + slot
  SWITCH_COUNT = SWITCH_SLOT_FIRST + neatocore::SCHEDULE_DAYS * neatocore::SCHEDULE_SLOTS_PER_DAY,
};
static const uint8_t USER_SETTING_SWITCH_COUNT = SWITCH_SCHEDULE;

enum NumberIdx : uint8_t {
  NUMBER_FILTER_CHANGE_INTERVAL,
  NUMBER_BRUSH_CHANGE_INTERVAL,
  NUMBER_DIRT_BIN_REMINDER,
  NUMBER_STALL_THRESHOLD,
  NUMBER_MANUAL_BRUSH_SPEED,
  NUMBER_MANUAL_VACUUM_SPEED,
  NUMBER_MANUAL_SIDE_BRUSH_POWER,
  NUMBER_COUNT,
};

enum SelectIdx : uint8_t {
  SELECT_NAVIGATION_MODE,
  SELECT_SOUND,
  SELECT_COUNT,
};

enum ButtonIdx : uint8_t {
  BUTTON_START_HOUSE_CLEAN,
  BUTTON_START_SPOT_CLEAN,
  BUTTON_PAUSE,
  BUTTON_STOP,
  BUTTON_RETURN_TO_BASE,
  BUTTON_LOCATE,
  BUTTON_PLAY_SOUND,
  BUTTON_CLEAR_ERRORS,
  BUTTON_RESTART_ROBOT,
  BUTTON_SHUT_DOWN_ROBOT,
  BUTTON_NEW_BATTERY,
  BUTTON_SYNC_ROBOT_CLOCK,
  BUTTON_START_MAPPING_RUN,
  BUTTON_COUNT,
};

class OpenNeatoComponent;

// -- Entity classes ------------------------------------------------------------

class OpenNeatoSwitch : public switch_::Switch, public Parented<OpenNeatoComponent> {
 public:
  void set_index(uint8_t index) { this->index_ = index; }
  // Hub-side restore of RESTORE_* modes; publishes the restored state.
  optional<bool> restore();

 protected:
  void write_state(bool state) override;
  uint8_t index_{0};
};

class OpenNeatoNumber : public number::Number, public Parented<OpenNeatoComponent> {
 public:
  void set_index(uint8_t index) { this->index_ = index; }
  void set_restore(bool restore, float initial) {
    this->restore_ = restore;
    this->initial_ = initial;
  }
  float restore();
  void save(float value);

 protected:
  void control(float value) override;
  uint8_t index_{0};
  bool restore_{false};
  float initial_{NAN};
  ESPPreferenceObject pref_;
};

class OpenNeatoSelect : public select::Select, public Parented<OpenNeatoComponent> {
 public:
  void set_index(uint8_t index) { this->index_ = index; }
  void set_restore(bool restore) { this->restore_ = restore; }
  size_t restore(size_t initial);
  void save(size_t index);

 protected:
  void control(size_t index) override;
  uint8_t index_{0};
  bool restore_{false};
  ESPPreferenceObject pref_;
};

class OpenNeatoButton : public button::Button, public Parented<OpenNeatoComponent> {
 public:
  void set_index(uint8_t index) { this->index_ = index; }

 protected:
  void press_action() override;
  uint8_t index_{0};
};

class OpenNeatoTime : public datetime::TimeEntity, public Parented<OpenNeatoComponent> {
 public:
  void set_slot(uint8_t slot) { this->slot_ = slot; }
  void set_initial(uint8_t hour, uint8_t minute) {
    this->hour_ = hour;
    this->minute_ = minute;
    this->second_ = 0;
  }
  void restore();

 protected:
  void control(const datetime::TimeCall &call) override;
  uint8_t slot_{0};
  ESPPreferenceObject pref_;
};

// -- UART adapter --------------------------------------------------------------

class UartTransport : public neatocore::Transport {
 public:
  explicit UartTransport(uart::UARTDevice *dev) : dev_(dev) {}
  int read_byte() override;
  void write(const std::string &data) override;

 protected:
  uart::UARTDevice *dev_;
};

// -- Hub -----------------------------------------------------------------------

class OpenNeatoComponent : public Component, public uart::UARTDevice {
 public:
  OpenNeatoComponent();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
#endif
  void set_pose_buffer_size(uint16_t size) { this->pose_buffer_size_ = size; }
  void set_robot_clock_utc(bool utc) { this->robot_clock_utc_ = utc; }
  void set_idle_poll_interval(uint32_t ms) { this->idle_poll_ms_ = ms; }

  void set_text_sensor(TextSensorIdx i, text_sensor::TextSensor *s) { this->text_[i] = s; }
  void set_sensor(SensorIdx i, sensor::Sensor *s) { this->sensors_[i] = s; }
  void set_binary_sensor(BinarySensorIdx i, binary_sensor::BinarySensor *s) { this->binary_[i] = s; }
  void set_switch(uint8_t i, OpenNeatoSwitch *s) { this->switches_[i] = s; }
  void set_number(NumberIdx i, OpenNeatoNumber *n) { this->numbers_[i] = n; }
  void set_select(SelectIdx i, OpenNeatoSelect *s) { this->selects_[i] = s; }
  void set_button(ButtonIdx i, OpenNeatoButton *b) { this->buttons_[i] = b; }
  void set_slot_time(uint8_t slot, OpenNeatoTime *t) { this->slot_times_[slot] = t; }
  void set_event(event::Event *e) { this->event_ = e; }

  // Entity callbacks.
  void on_switch(uint8_t index, bool state);
  void on_number(uint8_t index, float value);
  void on_select(uint8_t index, size_t option);
  void on_button(uint8_t index);
  void on_slot_time(uint8_t slot, uint8_t hour, uint8_t minute);

  // -- API action helpers (used by the actions in openneato.yaml) -------------
  // Synchronous actions: return success; last_error() holds the reason.
  bool api_clean(const std::string &action);
  bool api_play_sound(int32_t sound_id);
  bool api_set_user_setting(const std::string &key, const std::string &value);
  bool api_manual_move(int32_t left_mm, int32_t right_mm, int32_t speed);
  void api_pose_batch(int32_t since_seq, JsonObject root);
  bool last_ok() const { return this->last_error_.empty(); }
  const std::string &last_error() const { return this->last_error_; }

  // Round-trip actions: begin_*, then wait_until request_done(), then respond
  // with request_ok()/request_error()/request_fill(), then end_request().
  void begin_send_command(uint32_t call_id, const std::string &command);
  void begin_get_lidar(uint32_t call_id);
  bool request_done(uint32_t call_id) const;
  bool request_ok(uint32_t call_id) const;
  std::string request_error(uint32_t call_id) const;
  void request_fill(uint32_t call_id, JsonObject root) const;
  void end_request(uint32_t call_id);

  neatocore::NeatoClient &client() { return this->client_; }

 protected:
  struct PendingRequest {
    uint32_t call_id{0};
    uint32_t started_ms{0};
    bool done{false};
    bool ok{false};
    bool lidar{false};
    std::string error;
    std::string response;
    std::unique_ptr<neatocore::LdsScan> scan;
  };

  void set_error_(const char *error) { this->last_error_ = error; }
  std::shared_ptr<PendingRequest> find_request_(uint32_t call_id) const;
  std::shared_ptr<PendingRequest> new_request_(uint32_t call_id);

  // Polling.
  bool due_(uint32_t &last, uint32_t interval, uint32_t now);
  void poll_soon_();
  void run_state_cycle_();
  void handle_state_(const neatocore::RobotState &state, bool err_ok, const neatocore::ErrorData &err);
  void finish_cycle_();
  void poll_charger_();
  void poll_analog_();
  void poll_digital_();
  void poll_motors_();
  void poll_user_settings_();
  void poll_warranty_();
  void poll_version_();
  void sync_clock_();
  void tick_scheduler_();
  void trigger_scheduled_clean_(int day, int slot);
  void update_next_clean_();
  int64_t epoch_now_() const;
  bool time_valid_() const;

  // Publishing.
  void publish_version_(const neatocore::VersionData &v);
  void publish_charger_(const neatocore::ChargerData &c);
  void publish_motors_(const neatocore::MotorData &m);
  void publish_user_settings_(const neatocore::UserSettingsData &u);
  void publish_session_stats_();
  void publish_manual_();
  void publish_connected_(bool connected);
  void pub_text_(TextSensorIdx i, const std::string &value);
  void pub_sensor_(SensorIdx i, float value);
  void pub_binary_(BinarySensorIdx i, bool value);
  void pub_switch_(uint8_t i, bool value);
  void stop_refused_exploration_();
  void start_mapping_fallback_();
  void restore_after_mapping_();
  void fire_event_(const char *type);

  void manual_mode_(bool on);
  void manual_motors_();
  void user_setting_switch_(uint8_t index, bool state);
  void user_setting_number_(uint8_t index, float value);
  void apply_manual_setting_(uint8_t index, float value);

  UartTransport transport_;
  neatocore::NeatoClient client_;
  neatocore::ManualController manual_;
  neatocore::EventTracker events_;
  std::unique_ptr<neatocore::SessionRecorder> history_;
  neatocore::Schedule schedule_;
  neatocore::FiredGuard fired_;
  HighFrequencyLoopRequester high_freq_;

#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
  uint16_t pose_buffer_size_{256};
  bool robot_clock_utc_{true};
  uint32_t idle_poll_ms_{10000};
  uint32_t boot_id_{0};
  std::string last_error_;
  std::vector<std::shared_ptr<PendingRequest>> requests_;

  text_sensor::TextSensor *text_[TEXT_COUNT]{};
  sensor::Sensor *sensors_[SENSOR_COUNT]{};
  binary_sensor::BinarySensor *binary_[BINARY_COUNT]{};
  OpenNeatoSwitch *switches_[SWITCH_COUNT]{};
  OpenNeatoNumber *numbers_[NUMBER_COUNT]{};
  OpenNeatoSelect *selects_[SELECT_COUNT]{};
  OpenNeatoButton *buttons_[BUTTON_COUNT]{};
  OpenNeatoTime *slot_times_[neatocore::SCHEDULE_DAYS * neatocore::SCHEDULE_SLOTS_PER_DAY]{};
  event::Event *event_{nullptr};

  // Live robot state.
  bool identified_{false};
  bool connected_{false};
  bool active_{false};
  bool has_error_{false};
  int error_code_{200};
  std::string error_kind_;
  neatocore::MappingRun mapping_;
  bool eco_before_mapping_{false};
  bool eco_overridden_{false};
  std::string ui_state_;
  int battery_{-1};
  bool charging_{false};
  bool ext_power_{false};
  bool was_collecting_{false};

  // Poll bookkeeping (last run, millis).
  bool cycle_in_flight_{false};
  uint32_t last_cycle_{0};
  uint32_t last_charger_{0};
  uint32_t last_analog_{0};
  uint32_t last_digital_{0};
  uint32_t last_motors_{0};
  uint32_t last_user_settings_{0};
  uint32_t last_warranty_{0};
  uint32_t last_version_{0};
  uint32_t last_scheduler_{0};
  uint32_t last_stats_{0};
  uint32_t last_clock_sync_{0};
  uint32_t last_clock_check_{0};
  bool clock_synced_{false};
  bool clock_sync_requested_{false};
  bool initial_polls_done_{false};

  // Scheduler: clean pending after a restart-before-clean power cycle.
  bool pending_clean_after_restart_{false};
  uint32_t restart_issued_at_{0};
};

}  // namespace esphome::openneato
