#include "openneato.h"

#include <cinttypes>
#include <cmath>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::openneato {

static const char *const TAG = "openneato";

// SetUserSettings keys, indexed by SwitchIdx (< USER_SETTING_SWITCH_COUNT).
static const char *const USER_SETTING_KEYS[USER_SETTING_SWITCH_COUNT] = {
    "EcoMode", "IntenseClean", "BinFullDetect", "WallEnable", "ButtonClick", "Melodies", "Warnings", "StealthLED",
};

static const uint32_t ACTIVE_POLL_MS = neatocore::HISTORY_POSE_INTERVAL_MS;
static const uint32_t CHARGER_IDLE_MS = 30000;
static const uint32_t CHARGER_ACTIVE_MS = 15000;
static const uint32_t ANALOG_MS = 60000;
static const uint32_t DIGITAL_MS = 60000;
static const uint32_t MOTORS_MS = 60000;
static const uint32_t USER_SETTINGS_MS = 300000;
static const uint32_t WARRANTY_MS = 1800000;
static const uint32_t VERSION_MS = 3600000;
static const uint32_t STATS_MS = 5000;
static const uint32_t CLOCK_SYNC_MS = 4UL * 3600UL * 1000UL;
static const uint32_t CLOCK_RETRY_MS = 60000;
static const uint32_t REQUEST_GC_MS = 30000;
static const size_t BACKGROUND_QUEUE_LIMIT = 4;
static const float SECONDS_PER_DAY = 86400.0f;

static uint32_t clock_ms() { return millis(); }

// -- Entities ------------------------------------------------------------------

optional<bool> OpenNeatoSwitch::restore() {
  auto initial = this->get_initial_state_with_restore_mode();
  if (initial.has_value())
    this->publish_state(*initial);
  return initial;
}

void OpenNeatoSwitch::write_state(bool state) { this->parent_->on_switch(this->index_, state); }

float OpenNeatoNumber::restore() {
  float value = this->initial_;
  if (this->restore_) {
    this->pref_ = this->make_entity_preference<float>();
    float stored;
    if (this->pref_.load(&stored) && std::isfinite(stored) && stored >= this->traits.get_min_value() &&
        stored <= this->traits.get_max_value())
      value = stored;
  }
  if (std::isfinite(value))
    this->publish_state(value);
  return value;
}

void OpenNeatoNumber::save(float value) {
  this->publish_state(value);
  if (this->restore_)
    this->pref_.save(&value);
}

void OpenNeatoNumber::control(float value) { this->parent_->on_number(this->index_, value); }

size_t OpenNeatoSelect::restore(size_t initial) {
  size_t index = initial;
  if (this->restore_) {
    this->pref_ = this->make_entity_preference<uint32_t>();
    uint32_t stored;
    if (this->pref_.load(&stored) && this->has_index(stored))
      index = stored;
  }
  if (this->has_index(index))
    this->publish_state(index);
  return index;
}

void OpenNeatoSelect::save(size_t index) {
  this->publish_state(index);
  if (this->restore_) {
    uint32_t stored = index;
    this->pref_.save(&stored);
  }
}

void OpenNeatoSelect::control(size_t index) { this->parent_->on_select(this->index_, index); }

void OpenNeatoButton::press_action() { this->parent_->on_button(this->index_); }

void OpenNeatoTime::restore() {
  this->pref_ = this->make_entity_preference<datetime::TimeEntityRestoreState>();
  datetime::TimeEntityRestoreState stored;
  if (this->pref_.load(&stored) && stored.hour < 24 && stored.minute < 60) {
    this->hour_ = stored.hour;
    this->minute_ = stored.minute;
    this->second_ = 0;
  }
  this->publish_state();
}

void OpenNeatoTime::control(const datetime::TimeCall &call) {
  if (call.get_hour().has_value())
    this->hour_ = *call.get_hour();
  if (call.get_minute().has_value())
    this->minute_ = *call.get_minute();
  this->second_ = 0;
  this->publish_state();
  datetime::TimeEntityRestoreState stored{this->hour_, this->minute_, 0};
  this->pref_.save(&stored);
  this->parent_->on_slot_time(this->slot_, this->hour_, this->minute_);
}

// -- UART ----------------------------------------------------------------------

int UartTransport::read_byte() {
  uint8_t c;
  if (this->dev_->available() == 0 || !this->dev_->read_byte(&c))
    return -1;
  return c;
}

void UartTransport::write(const std::string &data) {
  this->dev_->write_array(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

// -- Lifecycle -------------------------------------------------------------------

OpenNeatoComponent::OpenNeatoComponent() : transport_(this), client_(&transport_, clock_ms), manual_(&client_) {}

void OpenNeatoComponent::setup() {
  this->boot_id_ = random_uint32();
  this->history_.reset(new neatocore::SessionRecorder(this->pose_buffer_size_));

  // Restored on-device settings.
  for (uint8_t i = SWITCH_SCHEDULE; i < SWITCH_COUNT; i++) {
    if (this->switches_[i] == nullptr || (i >= SWITCH_MANUAL_MODE && i <= SWITCH_MANUAL_SIDE_BRUSH))
      continue;
    auto state = this->switches_[i]->restore();
    bool on = state.has_value() && *state;
    if (i == SWITCH_SCHEDULE)
      this->schedule_.enabled = on;
    else if (i >= SWITCH_SLOT_FIRST)
      this->schedule_.slots[(i - SWITCH_SLOT_FIRST) / 2][(i - SWITCH_SLOT_FIRST) % 2].on = on;
  }
  for (uint8_t slot = 0; slot < neatocore::SCHEDULE_DAYS * neatocore::SCHEDULE_SLOTS_PER_DAY; slot++) {
    if (this->slot_times_[slot] == nullptr)
      continue;
    this->slot_times_[slot]->restore();
    auto &s = this->schedule_.slots[slot / 2][slot % 2];
    s.hour = this->slot_times_[slot]->hour;
    s.minute = this->slot_times_[slot]->minute;
  }
  for (uint8_t i = NUMBER_STALL_THRESHOLD; i < NUMBER_COUNT; i++) {
    if (this->numbers_[i] != nullptr)
      this->apply_manual_setting_(i, this->numbers_[i]->restore());
  }
  if (this->selects_[SELECT_NAVIGATION_MODE] != nullptr)
    this->selects_[SELECT_NAVIGATION_MODE]->restore(0);
  if (this->selects_[SELECT_SOUND] != nullptr)
    this->selects_[SELECT_SOUND]->restore(neatocore::SOUND_ALERT);

  this->publish_manual_();
  this->publish_connected_(false);

  this->client_.on_identified([this](const neatocore::VersionData &v) {
    ESP_LOGI(TAG, "Robot identified: %s (serial %s, firmware %s, supported: %s)", v.model_name.c_str(),
             v.serial_number.c_str(), v.software_version.c_str(), YESNO(neatocore::is_supported_model(v.model_name)));
    if (!this->client_.has_skey()) {
      ESP_LOGW(TAG, "Could not compute the SetEvent key; cleaning commands will fail");
    }
    this->identified_ = true;
    this->publish_version_(v);
    this->publish_connected_(true);
    // First round of polls right away; the regular cadence takes over afterwards.
    uint32_t now = millis();
    this->last_version_ = now;
    this->last_charger_ = this->last_user_settings_ = this->last_warranty_ = now;
    this->last_analog_ = this->last_digital_ = this->last_motors_ = now;
    this->poll_charger_();
    this->poll_user_settings_();
    this->poll_analog_();
    this->poll_digital_();
    this->poll_motors_();
    this->poll_warranty_();
    this->poll_soon_();
  });
  this->client_.set_nav_mode_getter([this]() -> std::string {
    auto *sel = this->selects_[SELECT_NAVIGATION_MODE];
    if (sel == nullptr || !sel->has_state())
      return "";
    return std::string(sel->current_option().c_str());
  });
  this->client_.on_clean_start([this]() { this->poll_soon_(); });
  this->client_.queue().set_logger(
      [this](const std::string &cmd, neatocore::CmdStatus status, uint32_t elapsed, size_t bytes) {
        if (status == neatocore::CmdStatus::SUCCESS) {
          ESP_LOGV(TAG, "%s: %" PRIu32 " ms, %u bytes", cmd.c_str(), elapsed, static_cast<unsigned>(bytes));
          if (this->identified_ && !this->connected_)
            this->publish_connected_(true);
          return;
        }
        ESP_LOGW(TAG, "%s: %s after %" PRIu32 " ms", cmd.c_str(), neatocore::cmd_status_str(status), elapsed);
        if (status == neatocore::CmdStatus::TIMEOUT)
          this->publish_connected_(false);
      });
  this->manual_.on_change([this]() { this->publish_manual_(); });

#ifdef USE_TIME
  if (this->time_ != nullptr)
    this->time_->add_on_time_sync_callback([this]() {
      if (!this->clock_synced_)
        this->clock_sync_requested_ = true;
      this->update_next_clean_();
    });
#endif
  this->update_next_clean_();
}

void OpenNeatoComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "OpenNeato:\n"
                "  Pose buffer: %u entries\n"
                "  Robot clock: %s\n"
                "  Idle poll interval: %" PRIu32 " ms",
                this->pose_buffer_size_, this->robot_clock_utc_ ? "UTC" : "local", this->idle_poll_ms_);
#ifdef USE_TIME
  ESP_LOGCONFIG(TAG, "  Time source: %s", this->time_ != nullptr ? "yes" : "no (schedule and clock sync disabled)");
#endif
}

bool OpenNeatoComponent::due_(uint32_t &last, uint32_t interval, uint32_t now) {
  if (now - last < interval)
    return false;
  last = now;
  return true;
}

void OpenNeatoComponent::poll_soon_() {
  // Next state cycle within ~0.5 s.
  this->last_cycle_ = millis() - ACTIVE_POLL_MS * 4;
  this->active_ = true;
}

void OpenNeatoComponent::loop() {
  uint32_t now = millis();
  this->client_.tick();
  this->manual_.tick(now);
  // Drain the UART promptly while a (possibly 6 KB LIDAR) reply is streaming in.
  if (this->client_.queue().is_waiting_response())
    this->high_freq_.start();
  else
    this->high_freq_.stop();

  for (auto it = this->requests_.begin(); it != this->requests_.end();) {
    if (now - (*it)->started_ms > REQUEST_GC_MS)
      it = this->requests_.erase(it);
    else
      ++it;
  }

  if (!this->identified_)
    return;

  bool collecting = this->history_->collecting();
  uint32_t interval = (this->active_ || collecting || this->manual_.active()) ? ACTIVE_POLL_MS : this->idle_poll_ms_;
  if (!this->cycle_in_flight_ && now - this->last_cycle_ >= interval)
    this->run_state_cycle_();

  if (this->client_.queue().depth() < BACKGROUND_QUEUE_LIMIT) {
    if (this->due_(this->last_charger_, this->active_ ? CHARGER_ACTIVE_MS : CHARGER_IDLE_MS, now))
      this->poll_charger_();
    else if (this->due_(this->last_analog_, ANALOG_MS, now))
      this->poll_analog_();
    else if (!this->manual_.active() && this->due_(this->last_digital_, DIGITAL_MS, now))
      this->poll_digital_();
    else if (!collecting && !this->manual_.active() && this->due_(this->last_motors_, MOTORS_MS, now))
      this->poll_motors_();
    else if (this->due_(this->last_user_settings_, USER_SETTINGS_MS, now))
      this->poll_user_settings_();
    else if (this->due_(this->last_warranty_, WARRANTY_MS, now))
      this->poll_warranty_();
    else if (this->due_(this->last_version_, VERSION_MS, now))
      this->poll_version_();
  }

  if (this->due_(this->last_scheduler_, neatocore::SCHEDULE_CHECK_INTERVAL_MS, now))
    this->tick_scheduler_();
  if (collecting && this->due_(this->last_stats_, STATS_MS, now))
    this->publish_session_stats_();

  if (this->due_(this->last_clock_check_, 5000, now) && this->time_valid_()) {
    uint32_t since = now - this->last_clock_sync_;
    bool retry = !this->clock_synced_ && (this->last_clock_sync_ == 0 || since >= CLOCK_RETRY_MS);
    if (this->clock_sync_requested_ || retry || (this->clock_synced_ && since >= CLOCK_SYNC_MS))
      this->sync_clock_();
  }
}

// -- State cycle -----------------------------------------------------------------

void OpenNeatoComponent::run_state_cycle_() {
  this->cycle_in_flight_ = true;
  this->last_cycle_ = millis();
  this->client_.get_state([this](bool ok, const neatocore::RobotState &state) {
    if (!ok) {
      this->finish_cycle_();
      return;
    }
    this->client_.get_error(
        [this, state](bool err_ok, const neatocore::ErrorData &err) { this->handle_state_(state, err_ok, err); });
  });
}

void OpenNeatoComponent::finish_cycle_() { this->cycle_in_flight_ = false; }

void OpenNeatoComponent::handle_state_(const neatocore::RobotState &state, bool err_ok, const neatocore::ErrorData &err) {
  const std::string &ui = state.ui_state;
  this->ui_state_ = ui;
  this->pub_text_(TEXT_UI_STATE, ui);
  this->pub_text_(TEXT_ROBOT_STATE, state.robot_state);

  if (err_ok) {
    this->has_error_ = err.has_error;
    this->error_kind_ = err.kind;
    this->pub_text_(TEXT_ERROR_MESSAGE, err.display_message);
    this->pub_text_(TEXT_ERROR_KIND, err.kind);
    this->pub_sensor_(SENSOR_ERROR_CODE, err.error_code);
    this->pub_binary_(BINARY_PROBLEM, err.has_error);
    for (const char *ev : this->events_.update_error(err.has_error, err.error_code, err.kind))
      this->fire_event_(ev);
  }
  for (const char *ev : this->events_.update_state(ui, state.robot_state))
    this->fire_event_(ev);

  this->pub_text_(TEXT_ACTIVITY, neatocore::compute_activity(ui, this->manual_.active(), this->has_error_,
                                                         this->error_kind_, this->charging_, this->ext_power_));
  this->active_ = neatocore::is_active_state(ui);

  bool wants = this->history_->on_state(ui, state.robot_state, this->battery_, this->epoch_now_(), millis());
  if (this->history_->collecting() != this->was_collecting_) {
    this->was_collecting_ = this->history_->collecting();
    ESP_LOGI(TAG, "Cleaning session %s", this->was_collecting_ ? "started" : "ended");
    this->publish_session_stats_();
    if (!this->was_collecting_)
      this->last_charger_ = millis() - CHARGER_IDLE_MS;  // refresh battery soon
  }
  if (wants && this->history_->on_error(err_ok ? err.has_error : this->has_error_)) {
    this->client_.get_pose([this](bool pose_ok, const neatocore::Pose &pose) {
      if (!pose_ok) {
        this->finish_cycle_();
        return;
      }
      // Brush speed rides along with the pose so the replay only paints
      // stretches where the brush was turning.
      this->client_.get_motors([this, pose](bool motors_ok, const neatocore::MotorData &m) {
        if (motors_ok)
          this->publish_motors_(m);
        this->history_->on_pose(pose, motors_ok ? m.brush_rpm : -1);
        this->finish_cycle_();
      });
    });
    return;
  }
  this->finish_cycle_();
}

// -- Background polls --------------------------------------------------------------

void OpenNeatoComponent::poll_charger_() {
  this->client_.get_charger([this](bool ok, const neatocore::ChargerData &c) {
    if (ok)
      this->publish_charger_(c);
  });
}

void OpenNeatoComponent::poll_analog_() {
  this->client_.get_analog([this](bool ok, const neatocore::AnalogData &a) {
    if (!ok)
      return;
    if (a.has_current)
      this->pub_sensor_(SENSOR_BATTERY_CURRENT, a.battery_current_ma);
  });
}

void OpenNeatoComponent::poll_digital_() {
  this->client_.get_digital_sensors([this](bool ok, const neatocore::DigitalSensorData &d) {
    if (!ok)
      return;
    this->pub_binary_(BINARY_DUSTBIN, d.dustbin_in);
    if (this->manual_.active())
      return;  // the manual controller owns the safety sensors meanwhile
    this->pub_binary_(BINARY_WHEEL_LIFTED, d.left_wheel_extended || d.right_wheel_extended);
    this->pub_binary_(BINARY_FRONT_LEFT_BUMPER, d.l_front || d.l_lds);
    this->pub_binary_(BINARY_FRONT_RIGHT_BUMPER, d.r_front || d.r_lds);
    this->pub_binary_(BINARY_SIDE_LEFT_BUMPER, d.l_side);
    this->pub_binary_(BINARY_SIDE_RIGHT_BUMPER, d.r_side);
  });
}

void OpenNeatoComponent::poll_motors_() {
  this->client_.get_motors([this](bool ok, const neatocore::MotorData &m) {
    if (ok)
      this->publish_motors_(m);
  });
}

void OpenNeatoComponent::poll_user_settings_() {
  this->client_.get_user_settings([this](bool ok, const neatocore::UserSettingsData &u) {
    if (ok)
      this->publish_user_settings_(u);
  });
}

void OpenNeatoComponent::poll_warranty_() {
  this->client_.get_warranty([this](bool ok, const neatocore::WarrantyData &w) {
    if (!ok)
      return;
    this->pub_sensor_(SENSOR_BATTERY_CYCLES, w.battery_cycles);
    this->pub_sensor_(SENSOR_TOTAL_CLEANING_TIME, w.cleaning_time_s);
  });
}

void OpenNeatoComponent::poll_version_() {
  this->client_.get_version([this](bool ok, const neatocore::VersionData &v) {
    if (!ok)
      return;
    this->publish_version_(v);
    int64_t now = this->epoch_now_();
    if (v.time_utc > 0 && now > 0) {
      ESP_LOGD(TAG, "Robot clock offset: %" PRId64 " s", v.time_utc - now);
    }
  });
}

// -- Clock sync ------------------------------------------------------------------

bool OpenNeatoComponent::time_valid_() const {
#ifdef USE_TIME
  return this->time_ != nullptr && this->time_->now().is_valid();
#else
  return false;
#endif
}

int64_t OpenNeatoComponent::epoch_now_() const {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    auto now = this->time_->utcnow();
    if (now.is_valid())
      return now.timestamp;
  }
#endif
  return 0;
}

void OpenNeatoComponent::sync_clock_() {
#ifdef USE_TIME
  this->clock_sync_requested_ = false;
  this->last_clock_sync_ = millis();
  if (this->last_clock_sync_ == 0)
    this->last_clock_sync_ = 1;
  if (!this->time_valid_() || !this->identified_) {
    ESP_LOGW(TAG, "Cannot sync the robot clock: time or robot not available yet");
    return;
  }
  ESPTime t = this->robot_clock_utc_ ? this->time_->utcnow() : this->time_->now();
  std::string cmd = neatocore::build_set_time(t.year, t.month, t.day_of_month, t.hour, t.minute, t.second);
  this->client_.send_raw(cmd, [this, cmd](neatocore::CmdStatus status, const std::string &) {
    if (status == neatocore::CmdStatus::SUCCESS) {
      ESP_LOGI(TAG, "Robot clock set (%s)", cmd.c_str());
      this->clock_synced_ = true;
      this->client_.invalidate_version();
    } else if (status == neatocore::CmdStatus::UNSUPPORTED) {
      ESP_LOGW(TAG, "Robot does not support SetTime All; clock sync disabled until the next interval");
      this->clock_synced_ = true;
    }
  });
#endif
}

// -- Scheduler -------------------------------------------------------------------

void OpenNeatoComponent::tick_scheduler_() {
  this->update_next_clean_();
#ifdef USE_TIME
  if (!this->time_valid_() || !this->identified_)
    return;
  ESPTime local = this->time_->now();
  int day = neatocore::to_schedule_day(local.day_of_week - 1);
  int now_mins = local.hour * 60 + local.minute;
  this->fired_.reset_for_day(day);

  if (this->pending_clean_after_restart_) {
    if (millis() - this->restart_issued_at_ > neatocore::RESTART_BOOT_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Robot did not come back after the restart; scheduled clean abandoned");
      this->pending_clean_after_restart_ = false;
    } else {
      this->client_.get_state([this](bool ok, const neatocore::RobotState &state) {
        if (!ok || !this->pending_clean_after_restart_ || !neatocore::is_idle_state(state.ui_state))
          return;
        this->pending_clean_after_restart_ = false;
        ESP_LOGI(TAG, "Robot ready after restart, starting scheduled clean");
        this->client_.clean("house", [](bool started) {
          if (!started) {
            ESP_LOGW(TAG, "Scheduled clean failed to start");
          }
        });
      });
    }
  }

  if (!this->schedule_.enabled)
    return;
  for (int si = 0; si < neatocore::SCHEDULE_SLOTS_PER_DAY; si++) {
    const auto &slot = this->schedule_.slots[day][si];
    int sched_mins;
    if (!slot.on || !neatocore::is_action_due(slot.hour, slot.minute, now_mins, this->fired_.fired(si), sched_mins))
      continue;
    // Claim before going async so a slow reply cannot double-trigger.
    this->fired_.claim(si, sched_mins);
    auto *skip = this->switches_[SWITCH_SKIP_NEXT_CLEAN];
    if (skip != nullptr && skip->state) {
      ESP_LOGI(TAG, "Skipping scheduled clean %s %02d:%02d at user request", neatocore::SCHEDULE_DAY_NAMES[day],
               slot.hour, slot.minute);
      skip->publish_state(false);
      this->update_next_clean_();
      return;
    }
    this->trigger_scheduled_clean_(day, si);
    return;
  }
#endif
}

void OpenNeatoComponent::trigger_scheduled_clean_(int day, int slot) {
  auto *restart = this->switches_[SWITCH_RESTART_BEFORE_CLEAN];
  bool restart_first = restart != nullptr && restart->state;
  this->client_.get_state([this, day, slot, restart_first](bool ok, const neatocore::RobotState &state) {
    if (!ok) {
      ESP_LOGW(TAG, "GetState failed, scheduled clean will be retried");
      this->fired_.release(slot);
      return;
    }
    if (!neatocore::is_idle_state(state.ui_state)) {
      ESP_LOGI(TAG, "Robot busy (%s), skipping scheduled clean", state.ui_state.c_str());
      return;
    }
    if (restart_first) {
      ESP_LOGI(TAG, "Restarting robot before scheduled clean (%s slot %d)", neatocore::SCHEDULE_DAY_NAMES[day], slot + 1);
      this->client_.power_control("restart", [this](bool restarted) {
        if (!restarted) {
          ESP_LOGW(TAG, "Restart before clean failed");
          return;
        }
        this->pending_clean_after_restart_ = true;
        this->restart_issued_at_ = millis();
      });
      return;
    }
    if (!this->schedule_.enabled || !this->schedule_.slots[day][slot].on)
      return;
    ESP_LOGI(TAG, "Starting scheduled clean (%s slot %d)", neatocore::SCHEDULE_DAY_NAMES[day], slot + 1);
    this->client_.clean("house", [](bool started) {
      if (!started) {
        ESP_LOGW(TAG, "Scheduled clean failed to start");
      }
    });
  });
}

void OpenNeatoComponent::update_next_clean_() {
#ifdef USE_TIME
  if (this->time_valid_() && this->schedule_.enabled) {
    ESPTime local = this->time_->now();
    auto *skip = this->switches_[SWITCH_SKIP_NEXT_CLEAN];
    int skip_count = (skip != nullptr && skip->state) ? 1 : 0;
    auto next = neatocore::next_slot(this->schedule_, neatocore::to_schedule_day(local.day_of_week - 1),
                                 local.hour * 60 + local.minute, skip_count);
    if (next.found) {
      int64_t midnight = static_cast<int64_t>(local.timestamp) - (local.hour * 3600 + local.minute * 60 + local.second);
      this->pub_sensor_(SENSOR_NEXT_CLEAN,
                        static_cast<float>(midnight + next.day_offset * 86400LL + next.minutes * 60LL));
      return;
    }
  }
#endif
  this->pub_sensor_(SENSOR_NEXT_CLEAN, NAN);
}

// -- Publishing --------------------------------------------------------------------

void OpenNeatoComponent::pub_text_(TextSensorIdx i, const std::string &value) {
  auto *t = this->text_[i];
  if (t != nullptr && (!t->has_state() || t->state != value))
    t->publish_state(value);
}

void OpenNeatoComponent::pub_sensor_(SensorIdx i, float value) {
  auto *s = this->sensors_[i];
  if (s == nullptr)
    return;
  bool same = s->has_state() && ((std::isnan(value) && std::isnan(s->get_raw_state())) || s->get_raw_state() == value);
  if (!same)
    s->publish_state(value);
}

void OpenNeatoComponent::pub_binary_(BinarySensorIdx i, bool value) {
  if (this->binary_[i] != nullptr)
    this->binary_[i]->publish_state(value);
}

void OpenNeatoComponent::pub_switch_(uint8_t i, bool value) {
  if (this->switches_[i] != nullptr)
    this->switches_[i]->publish_state(value);
}

void OpenNeatoComponent::fire_event_(const char *type) {
  ESP_LOGI(TAG, "Robot event: %s", type);
  if (this->event_ != nullptr)
    this->event_->trigger(type);
}

void OpenNeatoComponent::publish_connected_(bool connected) {
  this->connected_ = connected;
  this->pub_binary_(BINARY_ROBOT_CONNECTED, connected);
}

void OpenNeatoComponent::publish_version_(const neatocore::VersionData &v) {
  this->pub_text_(TEXT_MODEL, v.model_name);
  this->pub_text_(TEXT_SERIAL_NUMBER, v.serial_number);
  this->pub_text_(TEXT_ROBOT_FIRMWARE, v.software_version);
  this->pub_text_(TEXT_LDS_FIRMWARE, v.lds_version);
  this->pub_text_(TEXT_MAIN_BOARD, v.main_board_version);
}

void OpenNeatoComponent::publish_charger_(const neatocore::ChargerData &c) {
  this->battery_ = c.fuel_percent;
  this->charging_ = c.charging_active;
  this->ext_power_ = c.ext_pwr_present;
  this->pub_sensor_(SENSOR_BATTERY, c.fuel_percent);
  this->pub_sensor_(SENSOR_BATTERY_VOLTAGE, c.v_batt);
  this->pub_sensor_(SENSOR_EXTERNAL_VOLTAGE, c.v_ext);
  this->pub_sensor_(SENSOR_BATTERY_TEMPERATURE, c.batt_temp_c >= 0 ? static_cast<float>(c.batt_temp_c) : NAN);
  this->pub_binary_(BINARY_CHARGING, c.charging_active);
  this->pub_binary_(BINARY_EXTERNAL_POWER, c.ext_pwr_present);
  this->pub_binary_(BINARY_BATTERY_OVERHEATING, c.battery_over_temp);
  this->pub_binary_(BINARY_BATTERY_FAILURE, c.battery_failure);
  if (!this->ui_state_.empty())
    this->pub_text_(TEXT_ACTIVITY, neatocore::compute_activity(this->ui_state_, this->manual_.active(), this->has_error_,
                                                           this->error_kind_, this->charging_, this->ext_power_));
  if (this->history_->collecting())
    this->publish_session_stats_();
}

void OpenNeatoComponent::publish_motors_(const neatocore::MotorData &m) {
  this->pub_sensor_(SENSOR_BRUSH_RPM, m.brush_rpm);
  this->pub_sensor_(SENSOR_VACUUM_RPM, m.vacuum_rpm);
  this->pub_sensor_(SENSOR_LEFT_WHEEL_LOAD, m.left_wheel_load);
  this->pub_sensor_(SENSOR_RIGHT_WHEEL_LOAD, m.right_wheel_load);
  this->pub_sensor_(SENSOR_LDS_RPM, m.laser_rpm);
}

void OpenNeatoComponent::publish_user_settings_(const neatocore::UserSettingsData &u) {
  const bool values[USER_SETTING_SWITCH_COUNT] = {u.eco_mode, u.intense_clean, u.bin_full_detect, u.wall_enable,
                                                  u.button_click, u.melodies, u.warnings, u.stealth_led};
  for (uint8_t i = 0; i < USER_SETTING_SWITCH_COUNT; i++)
    this->pub_switch_(i, values[i]);
  if (this->numbers_[NUMBER_FILTER_CHANGE_INTERVAL] != nullptr)
    this->numbers_[NUMBER_FILTER_CHANGE_INTERVAL]->publish_state(std::round(u.filter_change / SECONDS_PER_DAY));
  if (this->numbers_[NUMBER_BRUSH_CHANGE_INTERVAL] != nullptr)
    this->numbers_[NUMBER_BRUSH_CHANGE_INTERVAL]->publish_state(std::round(u.brush_change / SECONDS_PER_DAY));
  if (this->numbers_[NUMBER_DIRT_BIN_REMINDER] != nullptr)
    this->numbers_[NUMBER_DIRT_BIN_REMINDER]->publish_state(u.dirt_bin);
}

void OpenNeatoComponent::publish_session_stats_() {
  if (!this->history_->has_session())
    return;
  const auto &h = *this->history_;
  this->pub_sensor_(SENSOR_CLEAN_STARTED, h.start_epoch() > 0 ? static_cast<float>(h.start_epoch()) : NAN);
  this->pub_sensor_(SENSOR_CLEAN_DURATION, h.duration_s(this->epoch_now_(), millis()));
  this->pub_sensor_(SENSOR_CLEAN_AREA, h.area_m2());
  this->pub_sensor_(SENSOR_CLEAN_DISTANCE, h.distance_m());
  this->pub_sensor_(SENSOR_CLEAN_RECHARGES, h.recharges());
  int end = h.collecting() ? this->battery_ : h.battery_end();
  this->pub_sensor_(SENSOR_CLEAN_BATTERY_USED,
                    h.battery_start() >= 0 && end >= 0 ? static_cast<float>(h.battery_start() - end) : NAN);
}

void OpenNeatoComponent::publish_manual_() {
  bool active = this->manual_.active();
  this->pub_switch_(SWITCH_MANUAL_MODE, active);
  this->pub_switch_(SWITCH_MANUAL_BRUSH, active && this->manual_.brush_on());
  this->pub_switch_(SWITCH_MANUAL_VACUUM, active && this->manual_.vacuum_on());
  this->pub_switch_(SWITCH_MANUAL_SIDE_BRUSH, active && this->manual_.side_brush_on());
  const auto &s = this->manual_.safety();
  this->pub_binary_(BINARY_STALL_FRONT, active && s.stall_front);
  this->pub_binary_(BINARY_STALL_REAR, active && s.stall_rear);
  if (active) {
    this->pub_binary_(BINARY_WHEEL_LIFTED, s.wheel_lifted);
    this->pub_binary_(BINARY_FRONT_LEFT_BUMPER, s.bumper_front_left);
    this->pub_binary_(BINARY_FRONT_RIGHT_BUMPER, s.bumper_front_right);
    this->pub_binary_(BINARY_SIDE_LEFT_BUMPER, s.bumper_side_left);
    this->pub_binary_(BINARY_SIDE_RIGHT_BUMPER, s.bumper_side_right);
  }
}

// -- Entity callbacks --------------------------------------------------------------

void OpenNeatoComponent::on_switch(uint8_t index, bool state) {
  if (index < USER_SETTING_SWITCH_COUNT) {
    this->user_setting_switch_(index, state);
    return;
  }
  switch (index) {
    case SWITCH_MANUAL_MODE:
      this->manual_mode_(state);
      return;
    case SWITCH_MANUAL_BRUSH:
    case SWITCH_MANUAL_VACUUM:
    case SWITCH_MANUAL_SIDE_BRUSH: {
      if (!this->manual_.active()) {
        ESP_LOGW(TAG, "Motors can only be switched in manual mode");
        this->pub_switch_(index, false);
        return;
      }
      bool brush = index == SWITCH_MANUAL_BRUSH ? state : this->manual_.brush_on();
      bool vacuum = index == SWITCH_MANUAL_VACUUM ? state : this->manual_.vacuum_on();
      bool side = index == SWITCH_MANUAL_SIDE_BRUSH ? state : this->manual_.side_brush_on();
      this->manual_.set_motors(brush, vacuum, side, [this](bool ok) {
        if (!ok) {
          ESP_LOGW(TAG, "Manual motor command failed");
        }
        this->publish_manual_();
      });
      return;
    }
    default:
      break;
  }
  // On-device settings: persisted by the switch's restore mode.
  this->pub_switch_(index, state);
  if (index == SWITCH_SCHEDULE)
    this->schedule_.enabled = state;
  else if (index >= SWITCH_SLOT_FIRST)
    this->schedule_.slots[(index - SWITCH_SLOT_FIRST) / 2][(index - SWITCH_SLOT_FIRST) % 2].on = state;
  this->update_next_clean_();
}

void OpenNeatoComponent::user_setting_switch_(uint8_t index, bool state) {
  bool queued = this->client_.set_user_setting(USER_SETTING_KEYS[index], state ? "ON" : "OFF", [this, index, state](bool ok) {
    if (ok) {
      this->pub_switch_(index, state);
      this->last_user_settings_ = millis() - USER_SETTINGS_MS + 3000;  // re-read shortly
    } else {
      ESP_LOGW(TAG, "SetUserSettings %s failed", USER_SETTING_KEYS[index]);
    }
  });
  if (!queued) {
    ESP_LOGW(TAG, "Could not queue SetUserSettings %s", USER_SETTING_KEYS[index]);
  }
}

void OpenNeatoComponent::user_setting_number_(uint8_t index, float value) {
  std::string key;
  long robot_value;
  if (index == NUMBER_FILTER_CHANGE_INTERVAL) {
    key = "FilterChange";
    robot_value = std::lround(value * SECONDS_PER_DAY);
  } else if (index == NUMBER_BRUSH_CHANGE_INTERVAL) {
    key = "BrushChange";
    robot_value = std::lround(value * SECONDS_PER_DAY);
  } else {
    key = "DirtBin";
    robot_value = std::lround(value);
  }
  this->client_.set_user_setting(key, std::to_string(robot_value), [this, index, value, key](bool ok) {
    if (ok) {
      this->numbers_[index]->publish_state(value);
    } else {
      ESP_LOGW(TAG, "SetUserSettings %s failed", key.c_str());
    }
  });
}

void OpenNeatoComponent::on_number(uint8_t index, float value) {
  if (index <= NUMBER_DIRT_BIN_REMINDER) {
    this->user_setting_number_(index, value);
    return;
  }
  if (!std::isfinite(value))
    return;
  this->numbers_[index]->save(value);
  this->apply_manual_setting_(index, value);
}

void OpenNeatoComponent::apply_manual_setting_(uint8_t index, float value) {
  if (!std::isfinite(value))
    return;
  int v = static_cast<int>(std::lround(value));
  switch (index) {
    case NUMBER_STALL_THRESHOLD:
      this->manual_.set_stall_threshold(v);
      break;
    case NUMBER_MANUAL_BRUSH_SPEED:
      this->manual_.set_brush_rpm(v);
      break;
    case NUMBER_MANUAL_VACUUM_SPEED:
      this->manual_.set_vacuum_speed(v);
      break;
    case NUMBER_MANUAL_SIDE_BRUSH_POWER:
      this->manual_.set_side_brush_power(v);
      break;
    default:
      break;
  }
}

void OpenNeatoComponent::on_select(uint8_t index, size_t option) { this->selects_[index]->save(option); }

void OpenNeatoComponent::on_slot_time(uint8_t slot, uint8_t hour, uint8_t minute) {
  auto &s = this->schedule_.slots[slot / 2][slot % 2];
  s.hour = hour;
  s.minute = minute;
  this->update_next_clean_();
}

void OpenNeatoComponent::on_button(uint8_t index) {
  auto report = [](const char *what) {
    return [what](bool ok) {
      if (!ok) {
        ESP_LOGW(TAG, "%s failed", what);
      }
    };
  };
  switch (index) {
    case BUTTON_START_HOUSE_CLEAN:
      if (!this->api_clean("house")) {
        ESP_LOGW(TAG, "Start house clean: %s", this->last_error_.c_str());
      }
      break;
    case BUTTON_START_SPOT_CLEAN:
      if (!this->api_clean("spot")) {
        ESP_LOGW(TAG, "Start spot clean: %s", this->last_error_.c_str());
      }
      break;
    case BUTTON_START_MAPPING_RUN:
      if (!this->api_clean("explore")) {
        ESP_LOGW(TAG, "Start mapping run: %s", this->last_error_.c_str());
      }
      break;
    case BUTTON_PAUSE:
      if (!this->api_clean("pause")) {
        ESP_LOGW(TAG, "Pause: %s", this->last_error_.c_str());
      }
      break;
    case BUTTON_STOP:
      if (!this->api_clean("stop")) {
        ESP_LOGW(TAG, "Stop: %s", this->last_error_.c_str());
      }
      break;
    case BUTTON_RETURN_TO_BASE:
      if (!this->api_clean("dock")) {
        ESP_LOGW(TAG, "Return to base: %s", this->last_error_.c_str());
      }
      break;
    case BUTTON_LOCATE:
      this->client_.play_sound(neatocore::SOUND_ALERT, report("Locate"));
      break;
    case BUTTON_PLAY_SOUND: {
      auto *sel = this->selects_[SELECT_SOUND];
      int id = sel != nullptr && sel->has_state() ? static_cast<int>(sel->active_index().value_or(0))
                                                   : neatocore::SOUND_ALERT;
      this->client_.play_sound(id, report("Play sound"));
      break;
    }
    case BUTTON_CLEAR_ERRORS:
      this->client_.clear_errors(report("Clear errors"));
      this->poll_soon_();
      break;
    case BUTTON_RESTART_ROBOT:
      this->client_.power_control("restart", report("Restart robot"));
      break;
    case BUTTON_SHUT_DOWN_ROBOT:
      this->client_.power_control("shutdown", report("Shut down robot"));
      break;
    case BUTTON_NEW_BATTERY:
      this->client_.new_battery(report("New battery"));
      break;
    case BUTTON_SYNC_ROBOT_CLOCK:
      if (!this->time_valid_()) {
        ESP_LOGW(TAG, "Cannot sync the robot clock: no valid time source");
      } else {
        this->clock_sync_requested_ = true;
      }
      break;
    default:
      break;
  }
}

void OpenNeatoComponent::manual_mode_(bool on) {
  if (on && this->history_->collecting()) {
    ESP_LOGW(TAG, "Stop the cleaning run before entering manual mode");
    this->publish_manual_();
    return;
  }
  bool accepted = this->manual_.enable(on, millis(), [this, on](bool ok) {
    ESP_LOGI(TAG, "Manual mode %s%s", on ? "enable" : "disable", ok ? "d" : " failed");
    this->publish_manual_();
    this->poll_soon_();
  });
  if (!accepted)
    this->publish_manual_();
}

// -- API actions -----------------------------------------------------------------

bool OpenNeatoComponent::api_clean(const std::string &action) {
  if (action != "house" && action != "spot" && action != "pause" && action != "stop" && action != "dock" &&
      action != "explore") {
    this->set_error_("action must be one of house, spot, pause, stop, dock, explore");
    return false;
  }
  if (!this->client_.has_skey()) {
    this->set_error_("robot not identified yet (no SetEvent key)");
    return false;
  }
  if (this->manual_.active()) {
    this->set_error_("manual mode is active");
    return false;
  }
  if (!this->client_.clean(action, [action](bool ok) {
        if (!ok) {
          ESP_LOGW(TAG, "clean %s: robot did not accept the command", action.c_str());
        }
      })) {
    this->set_error_("serial queue full");
    return false;
  }
  if (action == "explore")
    this->history_->expect_mode("explore", millis());
  this->poll_soon_();
  this->last_error_.clear();
  return true;
}

bool OpenNeatoComponent::api_play_sound(int32_t sound_id) {
  if (sound_id < 0 || sound_id >= neatocore::SOUND_COUNT) {
    this->set_error_("sound_id must be 0-20");
    return false;
  }
  if (!this->client_.play_sound(sound_id)) {
    this->set_error_("serial queue full");
    return false;
  }
  this->last_error_.clear();
  return true;
}

bool OpenNeatoComponent::api_set_user_setting(const std::string &key, const std::string &value) {
  if (!this->client_.set_user_setting(key, value, [this](bool ok) {
        if (ok)
          this->last_user_settings_ = millis() - USER_SETTINGS_MS + 1000;
      })) {
    this->set_error_("invalid key/value or serial queue full");
    return false;
  }
  this->last_error_.clear();
  return true;
}

bool OpenNeatoComponent::api_manual_move(int32_t left_mm, int32_t right_mm, int32_t speed) {
  switch (this->manual_.move(left_mm, right_mm, speed, millis())) {
    case neatocore::MoveResult::OK:
      this->last_error_.clear();
      return true;
    case neatocore::MoveResult::INACTIVE:
      this->set_error_("inactive");
      return false;
    case neatocore::MoveResult::BLOCKED:
      this->set_error_("blocked");
      return false;
    case neatocore::MoveResult::QUEUE_FULL:
      this->set_error_("serial queue full");
      return false;
  }
  return false;
}

void OpenNeatoComponent::api_pose_batch(int32_t since_seq, JsonObject root) {
  const auto &h = *this->history_;
  bool more = false;
  std::string entries = h.entries_json(since_seq < 0 ? 0 : static_cast<uint32_t>(since_seq), more);
  root["boot"] = this->boot_id_;
  root["seq"] = h.ring().last_seq();
  root["first_seq"] = h.ring().first_seq();
  root["more"] = more;
  root["current"] = serialized(h.current_json());
  root["previous"] = serialized(h.previous_json());
  root["entries"] = serialized(entries);
}

std::shared_ptr<OpenNeatoComponent::PendingRequest> OpenNeatoComponent::find_request_(uint32_t call_id) const {
  for (const auto &r : this->requests_) {
    if (r->call_id == call_id)
      return r;
  }
  return nullptr;
}

std::shared_ptr<OpenNeatoComponent::PendingRequest> OpenNeatoComponent::new_request_(uint32_t call_id) {
  auto req = std::make_shared<PendingRequest>();
  req->call_id = call_id;
  req->started_ms = millis();
  // call_id 0 = caller wants no response: run it but do not track it.
  if (call_id != 0)
    this->requests_.push_back(req);
  return req;
}

void OpenNeatoComponent::begin_send_command(uint32_t call_id, const std::string &command) {
  auto req = this->new_request_(call_id);
  bool queued = this->client_.send_raw(command, [req](neatocore::CmdStatus status, const std::string &raw) {
    req->ok = status == neatocore::CmdStatus::SUCCESS;
    req->error = req->ok ? "" : neatocore::cmd_status_str(status);
    req->response = raw;
    req->done = true;
  });
  if (!queued && !req->done) {
    req->error = "invalid command or serial queue full";
    req->done = true;
  }
}

void OpenNeatoComponent::begin_get_lidar(uint32_t call_id) {
  auto req = this->new_request_(call_id);
  req->lidar = true;
  this->client_.get_lds_scan([req](bool ok, std::unique_ptr<neatocore::LdsScan> scan) {
    req->ok = ok;
    req->error = ok ? "" : "LIDAR scan failed";
    req->scan = std::move(scan);
    req->done = true;
  });
}

bool OpenNeatoComponent::request_done(uint32_t call_id) const {
  auto req = this->find_request_(call_id);
  return req == nullptr || req->done;
}

bool OpenNeatoComponent::request_ok(uint32_t call_id) const {
  auto req = this->find_request_(call_id);
  return req != nullptr && req->done && req->ok;
}

std::string OpenNeatoComponent::request_error(uint32_t call_id) const {
  auto req = this->find_request_(call_id);
  if (req == nullptr)
    return "request expired";
  if (!req->done)
    return "timeout waiting for the robot";
  return req->error;
}

void OpenNeatoComponent::request_fill(uint32_t call_id, JsonObject root) const {
  auto req = this->find_request_(call_id);
  if (req == nullptr || !req->done || !req->ok)
    return;
  if (!req->lidar) {
    root["response"] = req->response;
    return;
  }
  if (req->scan == nullptr)
    return;
  root["rotationSpeed"] = req->scan->rotation_speed;
  root["validPoints"] = req->scan->valid_points;
  root["dist"] = serialized(neatocore::lds_array_json(req->scan->dist));
  root["intensity"] = serialized(neatocore::lds_array_json(req->scan->intensity));
  root["error"] = serialized(neatocore::lds_array_json(req->scan->error));
}

void OpenNeatoComponent::end_request(uint32_t call_id) {
  for (auto it = this->requests_.begin(); it != this->requests_.end(); ++it) {
    if ((*it)->call_id == call_id) {
      this->requests_.erase(it);
      return;
    }
  }
}

}  // namespace esphome::openneato
