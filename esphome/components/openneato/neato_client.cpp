// Ported from OpenNeato firmware/src/neato_serial.cpp (MIT)
#include "neato_client.h"

namespace neatocore {

NeatoClient::NeatoClient(Transport *transport, ClockFn clock)
    : clock_(clock),
      queue_(transport),
      version_cache_(clock, CACHE_TTL_SLOW,
                     [this](AsyncCache<VersionData>::Callback cb) {
                       this->fetch_<VersionData>(NEATO_CMD_GET_VERSION, parse_version, std::move(cb));
                     }),
      charger_cache_(clock, CACHE_TTL_CHARGER,
                     [this](AsyncCache<ChargerData>::Callback cb) {
                       this->fetch_<ChargerData>(NEATO_CMD_GET_CHARGER, parse_charger, std::move(cb));
                     }),
      analog_cache_(clock, CACHE_TTL_SENSORS,
                    [this](AsyncCache<AnalogData>::Callback cb) {
                      this->fetch_<AnalogData>(NEATO_CMD_GET_ANALOG_SENSORS, parse_analog, std::move(cb));
                    }),
      warranty_cache_(clock, CACHE_TTL_SLOW,
                      [this](AsyncCache<WarrantyData>::Callback cb) {
                        this->fetch_<WarrantyData>(NEATO_CMD_GET_WARRANTY, parse_warranty, std::move(cb));
                      }),
      user_settings_cache_(clock, CACHE_TTL_SLOW,
                           [this](AsyncCache<UserSettingsData>::Callback cb) {
                             this->fetch_<UserSettingsData>(NEATO_CMD_GET_USER_SETTINGS, parse_user_settings,
                                                            std::move(cb));
                           }),
      state_cache_(clock, CACHE_TTL_STATE,
                   [this](AsyncCache<RobotState>::Callback cb) {
                     this->fetch_<RobotState>(NEATO_CMD_GET_STATE, parse_state, std::move(cb));
                   }),
      error_cache_(clock, CACHE_TTL_STATE,
                   [this](AsyncCache<ErrorData>::Callback cb) {
                     this->fetch_<ErrorData>(NEATO_CMD_GET_ERR, parse_error, std::move(cb));
                   }),
      pose_cache_(clock, CACHE_TTL_SENSORS,
                  [this](AsyncCache<Pose>::Callback cb) {
                    this->fetch_<Pose>(NEATO_CMD_GET_ROBOT_POS_SMOOTH, parse_pose, std::move(cb));
                  }),
      digital_cache_(clock, CACHE_TTL_SENSORS,
                     [this](AsyncCache<DigitalSensorData>::Callback cb) {
                       this->fetch_<DigitalSensorData>(NEATO_CMD_GET_DIGITAL_SENSORS, parse_digital_sensors,
                                                       std::move(cb));
                     }),
      motor_cache_(clock, CACHE_TTL_SENSORS, [this](AsyncCache<MotorData>::Callback cb) {
        this->fetch_<MotorData>(NEATO_CMD_GET_MOTORS, parse_motors, std::move(cb));
      }) {}

void NeatoClient::tick() {
  uint32_t now = this->clock_();
  // SKey bootstrap: first attempt right away, then exponential backoff (the
  // robot may still be booting when the bridge comes up).
  if (this->identifying_ && !this->identify_in_flight_ &&
      static_cast<int32_t>(now - this->identify_retry_at_) >= 0) {
    this->start_identify_();
  }
  this->queue_.tick(now);
}

void NeatoClient::start_identify_() {
  this->identify_in_flight_ = true;
  this->version_cache_.refresh([this](bool ok, const VersionData &v) {
    this->identify_in_flight_ = false;
    if (!ok || v.serial_number.empty()) {
      this->identify_retry_at_ = this->clock_() + this->identify_retry_delay_;
      this->identify_retry_delay_ = this->identify_retry_delay_ * 2 < SKEY_RETRY_MAX_MS
                                        ? this->identify_retry_delay_ * 2
                                        : SKEY_RETRY_MAX_MS;
      return;
    }
    this->identifying_ = false;
    this->identify_retry_delay_ = SKEY_RETRY_INITIAL_MS;
    this->skey_ = compute_skey(v.serial_number);
    if (this->identified_cb_)
      this->identified_cb_(v);
  });
}

void NeatoClient::get_state(std::function<void(bool, const RobotState &)> cb) {
  if (!this->manual_active_) {
    this->state_cache_.get(std::move(cb));
    return;
  }
  // The robot reports TESTMODE during manual clean; expose the pseudo-state.
  this->state_cache_.get([this, cb](bool ok, const RobotState &data) {
    if (!cb)
      return;
    if (!ok || !this->manual_active_) {
      cb(ok, data);
      return;
    }
    RobotState patched = data;
    patched.ui_state = "UIMGR_STATE_MANUALCLEANING";
    cb(true, patched);
  });
}

void NeatoClient::get_digital_sensors(std::function<void(bool, const DigitalSensorData &)> cb, Priority p) {
  if (p == PRIORITY_NORMAL)
    this->digital_cache_.get(std::move(cb));
  else
    this->fetch_<DigitalSensorData>(NEATO_CMD_GET_DIGITAL_SENSORS, parse_digital_sensors, std::move(cb), p);
}

void NeatoClient::get_motors(std::function<void(bool, const MotorData &)> cb, Priority p) {
  if (p == PRIORITY_NORMAL)
    this->motor_cache_.get(std::move(cb));
  else
    this->fetch_<MotorData>(NEATO_CMD_GET_MOTORS, parse_motors, std::move(cb), p);
}

void NeatoClient::get_lds_scan(std::function<void(bool, std::unique_ptr<LdsScan>)> cb) {
  this->queue_.enqueue(NEATO_CMD_GET_LDS_SCAN, [cb](CmdStatus status, const std::string &raw) {
    std::unique_ptr<LdsScan> scan(new LdsScan());
    bool ok = status == CmdStatus::SUCCESS && parse_lds_scan(raw, *scan);
    if (cb)
      cb(ok, ok ? std::move(scan) : nullptr);
  });
}

void NeatoClient::invalidate_state() {
  this->state_cache_.invalidate();
  this->error_cache_.invalidate();
}

bool NeatoClient::enqueue_action_(const std::string &cmd, DoneCallback cb, Priority p, bool wait) {
  return this->queue_.enqueue(
      cmd,
      [cb](CmdStatus status, const std::string &) {
        if (cb)
          cb(status == CmdStatus::SUCCESS);
      },
      p, wait);
}

bool NeatoClient::start_new_clean_(const char *event, DoneCallback cb, bool house) {
  this->invalidate_state();
  if (this->clean_start_cb_)
    this->clean_start_cb_();
  if (house && this->nav_mode_getter_) {
    std::string mode = this->nav_mode_getter_();
    // Fire-and-forget; the house clean proceeds regardless.
    if (!mode.empty() && mode != "Normal")
      this->queue_.enqueue(std::string(NEATO_CMD_SET_NAVIGATION_MODE) + " " + mode, nullptr, PRIORITY_HIGH);
  }
  return this->enqueue_action_(build_set_event(event, this->skey_), std::move(cb), PRIORITY_HIGH);
}

bool NeatoClient::clean(const std::string &action, DoneCallback cb) {
  // All cleaning control goes through SetEvent (the authenticated app/cloud
  // event API), which keeps the map/localization across pause/resume.
  if (!this->has_skey()) {
    if (cb)
      cb(false);
    return false;
  }
  const RobotState *cached = this->cached_state();
  const std::string ui = cached != nullptr ? cached->ui_state : std::string();
  if (action == "dock" || action == "pause" || action == "stop") {
    const char *evt = action == "dock" ? NEATO_EVT_SEND_TO_BASE : (action == "pause" ? NEATO_EVT_PAUSE : NEATO_EVT_STOP);
    this->invalidate_state();
    return this->enqueue_action_(build_set_event(evt, this->skey_), std::move(cb), PRIORITY_HIGH);
  }
  if (action == "house" || action == "spot") {
    bool house = action == "house";
    // Type-aware resume: "house" only resumes a paused house clean, etc.
    if (contains(ui, house ? "HOUSECLEANINGPAUSED" : "SPOTCLEANINGPAUSED")) {
      this->invalidate_state();
      return this->enqueue_action_(build_set_event(NEATO_EVT_RESUME, this->skey_), std::move(cb), PRIORITY_HIGH);
    }
    return this->start_new_clean_(house ? NEATO_EVT_START_HOUSE : NEATO_EVT_START_SPOT, std::move(cb), house);
  }
  if (cb)
    cb(false);
  return false;
}

bool NeatoClient::play_sound(int sound_id, DoneCallback cb) {
  if (sound_id < 0 || sound_id >= SOUND_COUNT) {
    if (cb)
      cb(false);
    return false;
  }
  return this->enqueue_action_(NEATO_CMD_PLAY_SOUND + std::to_string(sound_id), std::move(cb));
}

bool NeatoClient::test_mode(bool enable, DoneCallback cb) {
  this->invalidate_state();
  // HIGH: a prerequisite for the CRITICAL/MEDIUM motor commands that follow.
  return this->enqueue_action_(enable ? NEATO_CMD_TEST_MODE_ON : NEATO_CMD_TEST_MODE_OFF, std::move(cb),
                               PRIORITY_HIGH);
}

bool NeatoClient::set_lds_rotation(bool on, DoneCallback cb) {
  return this->enqueue_action_(on ? NEATO_CMD_SET_LDS_ROTATION_ON : NEATO_CMD_SET_LDS_ROTATION_OFF, std::move(cb),
                               PRIORITY_HIGH);
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool NeatoClient::set_motor_wheels(int left_mm, int right_mm, int speed_mm_s, DoneCallback cb) {
  // "SetMotor 0 0 0" is ignored (robot coasts ~1 s): disable the wheels for an
  // immediate stop, then re-enable them for the next move.
  if (left_mm == 0 && right_mm == 0) {
    return this->queue_.enqueue(
        std::string(NEATO_CMD_SET_MOTOR) + " LWheelDisable RWheelDisable",
        [this, cb](CmdStatus, const std::string &) {
          // URGENT: must run before a move queued meanwhile, or that move is ignored.
          this->enqueue_action_(std::string(NEATO_CMD_SET_MOTOR) + " LWheelEnable RWheelEnable", cb,
                                PRIORITY_URGENT);
        },
        PRIORITY_CRITICAL);
  }
  left_mm = clamp_int(left_mm, -10000, 10000);
  right_mm = clamp_int(right_mm, -10000, 10000);
  speed_mm_s = clamp_int(speed_mm_s, 0, 300);
  return this->enqueue_action_(std::string(NEATO_CMD_SET_MOTOR) + " LWheelDist " + std::to_string(left_mm) +
                                   " RWheelDist " + std::to_string(right_mm) + " Speed " + std::to_string(speed_mm_s),
                               std::move(cb), PRIORITY_CRITICAL);
}

bool NeatoClient::set_motor_brush(int rpm, DoneCallback cb) {
  if (rpm <= 0)
    return this->enqueue_action_(std::string(NEATO_CMD_SET_MOTOR) + " BrushDisable", std::move(cb), PRIORITY_MEDIUM);
  return this->queue_.enqueue(
      std::string(NEATO_CMD_SET_MOTOR) + " BrushEnable",
      [this, rpm, cb](CmdStatus status, const std::string &) {
        if (status != CmdStatus::SUCCESS) {
          if (cb)
            cb(false);
          return;
        }
        this->enqueue_action_(std::string(NEATO_CMD_SET_MOTOR) + " Brush RPM " + std::to_string(rpm), cb,
                              PRIORITY_MEDIUM);
      },
      PRIORITY_MEDIUM);
}

bool NeatoClient::set_motor_vacuum(bool on, int speed_pct, DoneCallback cb) {
  std::string cmd = std::string(NEATO_CMD_SET_MOTOR) +
                    (on ? " VacuumOn VacuumSpeed " + std::to_string(speed_pct) : std::string(" VacuumOff"));
  return this->enqueue_action_(cmd, std::move(cb), PRIORITY_MEDIUM);
}

bool NeatoClient::set_motor_side_brush(bool on, int power_mw, DoneCallback cb) {
  if (!on) {
    return this->queue_.enqueue(
        std::string(NEATO_CMD_SET_MOTOR) + " SideBrushOff",
        [this, cb](CmdStatus, const std::string &) {
          this->enqueue_action_(std::string(NEATO_CMD_SET_MOTOR) + " SideBrushDisable", cb, PRIORITY_MEDIUM);
        },
        PRIORITY_MEDIUM);
  }
  return this->queue_.enqueue(
      std::string(NEATO_CMD_SET_MOTOR) + " SideBrushEnable",
      [this, power_mw, cb](CmdStatus status, const std::string &) {
        if (status != CmdStatus::SUCCESS) {
          if (cb)
            cb(false);
          return;
        }
        this->enqueue_action_(
            std::string(NEATO_CMD_SET_MOTOR) + " SideBrushOn SideBrushPower " + std::to_string(power_mw), cb,
            PRIORITY_MEDIUM);
      },
      PRIORITY_MEDIUM);
}

bool NeatoClient::set_user_setting(const std::string &key, const std::string &value, DoneCallback cb) {
  if (key.empty() || value.empty() || key.find_first_of(" \r\n") != std::string::npos ||
      value.find_first_of("\r\n") != std::string::npos) {
    if (cb)
      cb(false);
    return false;
  }
  this->user_settings_cache_.invalidate();
  return this->enqueue_action_(std::string(NEATO_CMD_SET_USER_SETTINGS) + " " + key + " " + value,
                               [this, cb](bool ok) {
                                 this->user_settings_cache_.invalidate();
                                 if (cb)
                                   cb(ok);
                               });
}

bool NeatoClient::power_control(const std::string &action, DoneCallback cb) {
  const char *cmd;
  if (action == "restart") {
    cmd = NEATO_CMD_POWER_CYCLE;
  } else if (action == "shutdown") {
    cmd = NEATO_CMD_SHUTDOWN;
  } else {
    if (cb)
      cb(false);
    return false;
  }
  // SetSystemMode needs TestMode On first; it is fire-and-forget because the
  // robot (and possibly the bridge's power) goes away.
  return this->test_mode(true, [this, cmd, cb](bool ok) {
    if (!ok) {
      if (cb)
        cb(false);
      return;
    }
    this->enqueue_action_(cmd, cb, PRIORITY_HIGH, false);
  });
}

bool NeatoClient::clear_errors(DoneCallback cb) {
  this->invalidate_state();
  return this->enqueue_action_(NEATO_CMD_CLEAR_ERRORS, std::move(cb));
}

bool NeatoClient::new_battery(DoneCallback cb) {
  return this->test_mode(true, [this, cb](bool ok) {
    if (!ok) {
      if (cb)
        cb(false);
      return;
    }
    this->queue_.enqueue(
        NEATO_CMD_NEW_BATTERY,
        [this, cb](CmdStatus status, const std::string &) {
          if (status != CmdStatus::SUCCESS) {
            this->test_mode(false, nullptr);  // best effort
            if (cb)
              cb(false);
            return;
          }
          this->test_mode(false, cb);
        },
        PRIORITY_HIGH);
  });
}

bool NeatoClient::send_raw(const std::string &command, std::function<void(CmdStatus, const std::string &)> cb) {
  if (command.empty() || command.find_first_of("\r\n") != std::string::npos) {
    if (cb)
      cb(CmdStatus::SERIAL_ERROR, "");
    return false;
  }
  return this->queue_.enqueue(command, std::move(cb));
}

}  // namespace neatocore
