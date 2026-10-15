// Ported from OpenNeato firmware/src/neato_commands.cpp (MIT)
#include "neato_protocol.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace neatocore {

const char *const SOUND_NAMES[SOUND_COUNT] = {
    "Waking up",
    "Starting cleaning",
    "Cleaning completed",
    "Attention needed",
    "Backing up into base",
    "Docking completed",
    "Test sound 1",
    "Test sound 2",
    "Test sound 3",
    "Test sound 4",
    "Test sound 5",
    "Exploring",
    "Shutting down",
    "Picked up",
    "Going to sleep",
    "Returning home",
    "User canceled cleaning",
    "User terminated cleaning",
    "Slipped off base",
    "Alert",
    "Thank you",
};

// -- Human-readable error/alert messages -------------------------------------

struct ErrorMessage {
  const char *token;
  const char *message;
};

static const ErrorMessage ERROR_MESSAGES[] = {
    {"UI_ERROR_CHECK_BATTERY_SWITCH", "Check the battery switch"},
    {"UI_ERROR_DISCONNECT_CHRG_CABLE", "Disconnect the charging cable"},
    {"UI_ERROR_DISCONNECT_USB_CABLE", "Disconnect the USB cable"},
    {"UI_ERROR_SCHED_OFF", "Schedule is disabled"},
    {"UI_ERROR_TIME_NOT_SET", "Clock is not set"},
    {"UI_ERROR_DUST_BIN_EMPTIED", "Dust bin removed during cleaning"},
    {"UI_ERROR_DUST_BIN_MISSING", "Dust bin is missing"},
    {"UI_ERROR_DUST_BIN_FULL", "Dust bin is full"},
    {"UI_ERROR_BATTERY_OVERTEMP", "Battery is too hot"},
    {"UI_ERROR_UNABLE_TO_RETURN_TO_BASE", "Could not return to base"},
    {"UI_ERROR_QA_FAIL", "Quality assurance test failed"},
    {"UI_ERROR_BUMPER_STUCK", "Bumper is stuck"},
    {"UI_ERROR_PICKED_UP", "Robot is picked up or tilted"},
    {"UI_ERROR_RECONNECT_FAILED", "Failed to reconnect"},
    {"UI_ERROR_LWHEEL_STUCK", "Left wheel is stuck"},
    {"UI_ERROR_RWHEEL_STUCK", "Right wheel is stuck"},
    {"UI_ERROR_LDS_JAMMED", "LIDAR turret is jammed"},
    {"UI_ERROR_LDS_DISCONNECTED", "LIDAR is disconnected"},
    {"UI_ERROR_LDS_MISSED_PACKETS", "LIDAR missed packets"},
    {"UI_ERROR_LDS_BAD_PACKETS", "LIDAR bad packets"},
    {"UI_ERROR_LDS_LASER_OVER_POWER", "LIDAR laser over power"},
    {"UI_ERROR_LDS_LASER_UNDER_POWER", "LIDAR laser under power"},
    {"UI_ERROR_BRUSH_STUCK", "Main brush is stuck"},
    {"UI_ERROR_BRUSH_OVERLOAD", "Main brush is overloaded"},
    {"UI_ERROR_VACUUM_STUCK", "Vacuum motor is stuck"},
    {"UI_ERROR_VACUUM_SLIP", "Vacuum motor is slipping"},
    {"UI_ERROR_BATTERY_CRITICAL", "Battery critically low"},
    {"UI_ERROR_BATTERY_OverVolt", "Battery over-voltage"},
    {"UI_ERROR_BATTERY_UnderVolt", "Battery under-voltage"},
    {"UI_ERROR_BATTERY_UnderCurrent", "Battery under-current"},
    {"UI_ERROR_BATTERY_Mismatch", "Battery mismatch detected"},
    {"UI_ERROR_BATTERY_LithiumAdapterFailure", "Lithium battery adapter failure"},
    {"UI_ERROR_BATTERY_UnderTemp", "Battery is too cold"},
    {"UI_ERROR_BATTERY_Unplugged", "Battery is unplugged"},
    {"UI_ERROR_BATTERY_NoThermistor", "Battery thermistor missing"},
    {"UI_ERROR_BATTERY_BattUnderVoltLithiumSafety", "Battery under-voltage safety cutoff"},
    {"UI_ERROR_BATTERY_InvalidSensor", "Battery sensor is invalid"},
    {"UI_ERROR_BATTERY_PermanentError", "Permanent battery error"},
    {"UI_ERROR_BATTERY_Fault", "Battery fault"},
    {"UI_ERROR_NAVIGATION_UndockingFailed", "Failed to undock from base"},
    {"UI_ERROR_NAVIGATION_Falling", "Robot detected a cliff"},
    {"UI_ERROR_NAVIGATION_PinkyCommsFail", "Navigation sensor communication failure"},
    {"UI_ERROR_NAVIGATION_NoMotionCommands", "No motion commands received"},
    {"UI_ERROR_NAVIGATION_BackDrop_LeftBump", "Rear drop sensor triggered with left bump"},
    {"UI_ERROR_NAVIGATION_BackDrop_FrontBump", "Rear drop sensor triggered with front bump"},
    {"UI_ERROR_NAVIGATION_BackDrop_WheelExtended", "Rear drop sensor triggered with wheel extended"},
    {"UI_ERROR_NAVIGATION_RightDrop_LeftBump", "Right drop sensor triggered with left bump"},
    {"UI_ERROR_NAVIGATION_NoExitsToGo", "No exits available to continue"},
    {"UI_ERROR_NAVIGATION_PathProblems_ReturningHome", "Path problems while returning home"},
    {"UI_ERROR_NAVIGATION_NoProgress", "Robot is stuck"},
    {"UI_ERROR_NAVIGATION_BadMagSensor", "Magnetic sensor error"},
    {"UI_ERROR_NAVIGATION_Origin_Unclean", "Could not clean starting area"},
    {"UI_ERROR_NAVIGATION_PathBlocked_GoingToZone", "Path to zone is blocked"},
    {"UI_ERROR_SHUTDOWN", "Robot is shutting down"},
    {"UI_ERROR_DFLT_APP", "Default application error"},
    {"UI_ERROR_CORRUPT_SCB", "Corrupt system configuration"},
    {"UI_ERROR_SCB_FLASH_READ", "System config flash read error"},
    {"UI_ERROR_SCB_SIGNATURE", "System config signature error"},
    {"UI_ERROR_SCB_LENGTH_MISMATCH", "System config length mismatch"},
    {"UI_ERROR_SCB_CHECKSUM", "System config checksum error"},
    {"UI_ERROR_SCB_VALIDATION", "System config validation error"},
    {"UI_ERROR_SCB_INTERFACE", "System config interface error"},
    {"UI_ERROR_HARDWARE_FAILURE", "Hardware failure"},
    {"UI_ERROR_DECK_DEBRIS", "Clear debris from brush deck"},
    {"UI_ERROR_RDROP_STUCK", "Right drop sensor is stuck"},
    {"UI_ERROR_LDROP_STUCK", "Left drop sensor is stuck"},
    {"UI_ERROR_UNABLE_TO_SEE", "Navigation sensors blocked"},
    {"UI_ERROR_TILTED_ON_CLEANING_STARTUP", "Robot was tilted at cleaning start"},
    {"UI_ERROR_SWUPDATE_FILEMISSING", "Firmware update file missing"},
    {"UI_ERROR_FLIGHT_SENSOR_DISCONNECTED", "Floor sensor disconnected"},
    {"UI_ERROR_WIFIPSWDORROUTERISSUE", "WiFi password or router issue"},
    {"UI_ERROR_CONNECTINGTOSERVER", "Could not connect to server"},
    {"UI_ERROR_TIMEDOUTCONNECTROUTER", "Timed out connecting to router"},
};

static const ErrorMessage ALERT_MESSAGES[] = {
    {"UI_ALERT_RETURN_TO_BASE_PWR", "Returning to base (low power)"},
    {"UI_ALERT_RETURN_TO_BASE", "Returning to base"},
    {"UI_ALERT_RETURN_TO_START", "Returning to start"},
    {"UI_ALERT_RETURN_TO_CHARGE", "Returning to charge"},
    {"UI_ALERT_DUST_BIN_FULL", "Dust bin full"},
    {"UI_ALERT_BUSY_CHARGING", "Busy charging"},
    {"UI_ALERT_OLD_ERROR", "Previous error cleared"},
    {"UI_ALERT_RECOVERING_LOCATION", "Recovering location"},
    {"UI_ALERT_INFO_THANK_YOU", "Cleaning complete"},
    {"UI_ALERT_LOG_READ_FAIL", "Failed to read log"},
    {"UI_ALERT_LOG_WRITE_FAIL", "Failed to write log"},
    {"UI_ALERT_USB_DISCONNECTED", "USB disconnected"},
    {"UI_ALERT_SWUPDATE_SUCCESS", "Firmware update successful"},
    {"UI_ALERT_SWUPDATE_FAIL", "Firmware update failed"},
    {"UI_ALERT_LOG_WRITE_SUCCESS", "Log saved successfully"},
    {"UI_ALERT_TIME_NOT_SET", "Clock is not set"},
    {"UI_ALERT_TIME_SET", "Clock has been set"},
    {"UI_ALERT_TIMER_SET", "Schedule timer set"},
    {"UI_ALERT_TIMER_REMOVED", "Schedule timer removed"},
    {"UI_ALERT_ENABLE_TIMER", "Schedule timer enabled"},
    {"UI_ALERT_CHARGING_POWER", "Charging via power adapter"},
    {"UI_ALERT_CHARGING_BASE", "Charging on base"},
    {"UI_ALERT_BATTERY_ChargeBaseCommErr", "Charge base communication error"},
    {"UI_ALERT_CONNECT_CHRG_CABLE", "Connect charging cable"},
    {"UI_ALERT_WAIT_FOR_POWER_SWITCH_DETECT", "Waiting for power switch"},
    {"UI_ALERT_LINKEDAPP", "App linked"},
    {"UI_ALERT_ORIGIN_UNCLEAN", "Could not clean starting area"},
    {"UI_ALERT_LOGUPLOAD_FAIL", "Log upload failed"},
    {"UI_ALERT_BRUSH_CHANGE", "Time to replace the brush"},
    {"UI_ALERT_FILTER_CHANGE", "Time to replace the filter"},
    {"UI_ALERT_PERSISTENT_RELOCALIZATION_FAIL", "Failed to relocalize on saved map"},
    {"UI_ALERT_TRAINING_MULTIPLE_FLOORPLANS_VALID", "Multiple floor plans found during training"},
    {"UI_ALERT_MULTIPLE_FLOORPLANS_VALID", "Multiple floor plans detected"},
    {"UI_ALERT_PM_LOAD_FAIL", "Failed to load persistent map"},
    {"UI_ALERT_PM_SETUP_FAIL", "Failed to set up persistent map"},
    {"UI_ALERT_ACQUIRING_PERSISTENT_MAP_IDS", "Acquiring map data"},
    {"UI_ALERT_CREATING_AND_UPLOADING_MAP", "Creating and saving map"},
    {"UI_ALERT_PM_START_CLEAN_FAIL", "Failed to start cleaning with map"},
    {"UI_ALERT_NAV_FLOORPLAN_NOT_CREATED", "Floor plan not yet created"},
    {"UI_ALERT_NAV_FLOORPLAN_ZONE_UNREACHABLE", "Zone is unreachable"},
    {"UI_ALERT_NAV_FLOORPLAN_ZONE_WRONG_FLOOR", "Zone is on a different floor"},
    {"UI_ALERT_TRAINING_MAP_SPARSE", "Training map is too sparse"},
};

static std::string lookup_display_message(const std::string &raw) {
  for (const auto &entry : ERROR_MESSAGES) {
    if (raw.find(entry.token) != std::string::npos)
      return entry.message;
  }
  for (const auto &entry : ALERT_MESSAGES) {
    if (raw.find(entry.token) != std::string::npos)
      return entry.message;
  }
  return "";
}

// -- String helpers ----------------------------------------------------------

std::string trim(const std::string &s) {
  size_t start = 0;
  size_t end = s.size();
  while (start < end && isspace(static_cast<unsigned char>(s[start])))
    start++;
  while (end > start && isspace(static_cast<unsigned char>(s[end - 1])))
    end--;
  return s.substr(start, end - start);
}

std::string format_float(float value, int decimals) {
  if (!std::isfinite(value))
    value = 0.0f;
  char buf[32];
  snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(value));
  return buf;
}

static int to_int(const std::string &s) { return static_cast<int>(strtol(s.c_str(), nullptr, 10)); }
static float to_float(const std::string &s) { return strtof(s.c_str(), nullptr); }

static bool on_off(const std::string &s) { return strcasecmp(s.c_str(), "ON") == 0; }

static void replace_all(std::string &s, char from, char to) {
  for (auto &c : s) {
    if (c == from)
      c = to;
  }
}

bool contains(const std::string &haystack, const char *needle) { return haystack.find(needle) != std::string::npos; }

// Find the value for `label` in a "Label,Value\r\n" response (label matched exactly after trim).
static bool find_csv_value(const std::string &raw, const char *label, std::string &value) {
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t line_end = raw.find('\n', pos);
    if (line_end == std::string::npos)
      line_end = raw.size();
    std::string line = trim(raw.substr(pos, line_end - pos));
    size_t comma = line.find(',');
    if (comma != std::string::npos && comma > 0) {
      if (trim(line.substr(0, comma)) == label) {
        value = trim(line.substr(comma + 1));
        while (!value.empty() && value.back() == ',')
          value.pop_back();
        return true;
      }
    }
    pos = line_end + 1;
  }
  return false;
}

static std::string csv_last_field(const std::string &value) {
  size_t comma = value.rfind(',');
  if (comma == std::string::npos)
    return value;
  return trim(value.substr(comma + 1));
}

// -- Parsers -----------------------------------------------------------------

bool parse_version(const std::string &raw, VersionData &out) {
  std::string val;
  if (find_csv_value(raw, "Model", val) || find_csv_value(raw, "Product Model", val) ||
      find_csv_value(raw, "ModelID", val)) {
    // "BotVacD7Connected,905-0415" -> name; "0,XV11" (numeric index) -> second part
    size_t comma = val.find(',');
    if (comma != std::string::npos && comma > 0) {
      std::string prefix = val.substr(0, comma);
      bool numeric = true;
      for (char c : prefix) {
        if (!isdigit(static_cast<unsigned char>(c))) {
          numeric = false;
          break;
        }
      }
      val = numeric ? val.substr(comma + 1) : prefix;
      val = trim(val);
      if (!val.empty() && val.back() == ',')
        val.pop_back();
    }
    out.model_name = val;
  }
  if (find_csv_value(raw, "Serial Number", val))
    out.serial_number = val;
  if (find_csv_value(raw, "Software", val)) {
    out.software_version = val;
    replace_all(out.software_version, ',', '.');
  }
  if (find_csv_value(raw, "LDS Software", val))
    out.lds_version = val;
  if (find_csv_value(raw, "MainBoard Version", val)) {
    out.main_board_version = val;
    replace_all(out.main_board_version, ',', '.');
  }
  // "Time UTC,Sat Apr 11 19:26:13 2026"
  if (find_csv_value(raw, "Time UTC", val)) {
    size_t space = val.find(' ');
    if (space != std::string::npos && space > 0) {
      std::string rest = val.substr(space + 1);
      static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      int mon = -1;
      for (int i = 0; i < 12 && rest.size() >= 4; i++) {
        if (strncasecmp(rest.c_str(), MONTHS[i], 3) == 0) {
          mon = i;
          break;
        }
      }
      int day = 0, hour = 0, min = 0, sec = 0, year = 0;
      if (mon >= 0 && sscanf(rest.c_str() + 4, "%d %d:%d:%d %d", &day, &hour, &min, &sec, &year) == 5 &&
          year >= 2020) {
        static const int CUM_DAYS[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        int y = year - 1;
        long days = 365L * (year - 1970) + (y / 4 - y / 100 + y / 400) - (1969 / 4 - 1969 / 100 + 1969 / 400);
        days += CUM_DAYS[mon] + day - 1;
        if (mon > 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
          days++;
        out.time_utc = static_cast<int64_t>(days) * 86400 + hour * 3600 + min * 60 + sec;
      }
    }
  }
  return !out.model_name.empty() || !out.software_version.empty();
}

bool parse_charger(const std::string &raw, ChargerData &out) {
  std::string val;
  if (find_csv_value(raw, "FuelPercent", val))
    out.fuel_percent = to_int(val);
  if (find_csv_value(raw, "BatteryOverTemp", val))
    out.battery_over_temp = to_int(val) != 0;
  if (find_csv_value(raw, "ChargingActive", val))
    out.charging_active = to_int(val) != 0;
  if (find_csv_value(raw, "BatteryFailure", val))
    out.battery_failure = to_int(val) != 0;
  if (find_csv_value(raw, "ExtPwrPresent", val))
    out.ext_pwr_present = to_int(val) != 0;
  if (find_csv_value(raw, "VBattV", val))
    out.v_batt = to_float(val);
  if (find_csv_value(raw, "VExtV", val))
    out.v_ext = to_float(val);
  if (find_csv_value(raw, "BattTempCAvg", val))
    out.batt_temp_c = to_int(val);
  return out.fuel_percent >= 0;
}

bool parse_analog(const std::string &raw, AnalogData &out) {
  std::string val;
  bool found = false;
  if (find_csv_value(raw, "BatteryVoltage", val)) {
    out.battery_voltage_v = to_float(csv_last_field(val)) / 1000.0f;
    found = true;
  }
  if (find_csv_value(raw, "BatteryCurrent", val)) {
    out.battery_current_ma = to_int(csv_last_field(val));
    out.has_current = true;
    found = true;
  }
  if (find_csv_value(raw, "BatteryTemperature", val)) {
    out.battery_temperature_c = to_float(csv_last_field(val)) / 1000.0f;
    out.has_temperature = true;
    found = true;
  }
  if (find_csv_value(raw, "ExternalVoltage", val)) {
    out.external_voltage_v = to_float(csv_last_field(val)) / 1000.0f;
    found = true;
  }
  return found;
}

bool parse_warranty(const std::string &raw, WarrantyData &out) {
  std::string val;
  bool found = false;
  if (find_csv_value(raw, "CumulativeCleaningTimeInSecs", val)) {
    out.cleaning_time_s = static_cast<int>(strtol(val.c_str(), nullptr, 16));
    found = true;
  }
  if (find_csv_value(raw, "CumulativeBatteryCycles", val)) {
    out.battery_cycles = static_cast<int>(strtol(val.c_str(), nullptr, 16));
    found = true;
  }
  return found;
}

bool parse_digital_sensors(const std::string &raw, DigitalSensorData &out) {
  std::string val;
  bool found = false;
  if (find_csv_value(raw, "SNSR_DC_JACK_IS_IN", val) || find_csv_value(raw, "SNSR_DC_JACK_CONNECT", val)) {
    out.dc_jack_in = to_int(val) != 0;
    found = true;
  }
  if (find_csv_value(raw, "SNSR_DUSTBIN_IS_IN", val))
    out.dustbin_in = to_int(val) != 0;
  if (find_csv_value(raw, "SNSR_LEFT_WHEEL_EXTENDED", val))
    out.left_wheel_extended = to_int(val) != 0;
  if (find_csv_value(raw, "SNSR_RIGHT_WHEEL_EXTENDED", val))
    out.right_wheel_extended = to_int(val) != 0;
  if (find_csv_value(raw, "LSIDEBIT", val))
    out.l_side = to_int(val) != 0;
  if (find_csv_value(raw, "LFRONTBIT", val))
    out.l_front = to_int(val) != 0;
  if (find_csv_value(raw, "LLDSBIT", val))
    out.l_lds = to_int(val) != 0;
  if (find_csv_value(raw, "RSIDEBIT", val))
    out.r_side = to_int(val) != 0;
  if (find_csv_value(raw, "RFRONTBIT", val))
    out.r_front = to_int(val) != 0;
  if (find_csv_value(raw, "RLDSBIT", val))
    out.r_lds = to_int(val) != 0;
  return found;
}

bool parse_motors(const std::string &raw, MotorData &out) {
  std::string val;
  bool found = false;
  if (find_csv_value(raw, "Brush_RPM", val)) {
    out.brush_rpm = to_int(val);
    found = true;
  }
  if (find_csv_value(raw, "Vacuum_RPM", val))
    out.vacuum_rpm = to_int(val);
  if (find_csv_value(raw, "LeftWheel_Load%", val))
    out.left_wheel_load = to_int(val);
  if (find_csv_value(raw, "RightWheel_Load%", val))
    out.right_wheel_load = to_int(val);
  if (find_csv_value(raw, "Laser_RPM", val))
    out.laser_rpm = to_int(val);
  return found;
}

static std::string value_after(const std::string &raw, const char *marker) {
  size_t pos = raw.find(marker);
  if (pos == std::string::npos)
    return "";
  size_t start = pos + strlen(marker);
  size_t end = raw.find('\n', start);
  if (end == std::string::npos)
    end = raw.size();
  return trim(raw.substr(start, end - start));
}

bool parse_state(const std::string &raw, RobotState &out) {
  out.ui_state = value_after(raw, "Current UI State is:");
  out.robot_state = value_after(raw, "Current Robot State is:");
  return !out.ui_state.empty();
}

bool parse_error(const std::string &raw, ErrorData &out) {
  out.has_error = false;
  out.error_code = 200;
  out.kind.clear();
  out.display_message.clear();
  std::string trimmed = trim(raw);
  // Scan "NNN - ..." code lines; 200 (UI_ALERT_INVALID) means none, keep scanning.
  size_t search_from = 0;
  while (search_from < trimmed.size()) {
    size_t dash = trimmed.find(" - ", search_from);
    if (dash == std::string::npos)
      break;
    size_t num_start = dash;
    while (num_start > 0 && isdigit(static_cast<unsigned char>(trimmed[num_start - 1])))
      num_start--;
    if (num_start < dash) {
      int code = to_int(trimmed.substr(num_start, dash - num_start));
      if (code > 0 && code != 200) {
        out.has_error = true;
        out.error_code = code;
        out.kind = (code >= 201 && code <= 242) ? "warning" : "error";
        out.display_message = lookup_display_message(trimmed);
        if (out.display_message.empty())
          out.display_message = "Robot reported error " + std::to_string(code);
        return true;
      }
    }
    search_from = dash + 3;
  }
  return true;
}

// Labels differ from SetUserSettings keys ("ClickSounds" vs ButtonClick, "LED" vs StealthLED).
bool parse_user_settings(const std::string &raw, UserSettingsData &out) {
  std::string val;
  bool found = false;
  if (find_csv_value(raw, "ClickSounds", val)) {
    out.button_click = on_off(val);
    found = true;
  }
  if (find_csv_value(raw, "Melody Sounds", val)) {
    out.melodies = on_off(val);
    found = true;
  }
  if (find_csv_value(raw, "Warning Sounds", val)) {
    out.warnings = on_off(val);
    found = true;
  }
  if (find_csv_value(raw, "Eco Mode", val)) {
    out.eco_mode = on_off(val);
    found = true;
  }
  if (find_csv_value(raw, "IntenseClean", val)) {
    out.intense_clean = on_off(val);
    found = true;
  }
  if (find_csv_value(raw, "Bin Full Detect", val)) {
    out.bin_full_detect = on_off(val);
    found = true;
  }
  if (find_csv_value(raw, "Wall Enable", val)) {
    out.wall_enable = on_off(val);
    found = true;
  }
  if (find_csv_value(raw, "LED", val)) {
    // LED ON = LEDs visible; stealth = LEDs hidden.
    out.stealth_led = strcasecmp(val.c_str(), "OFF") == 0;
    found = true;
  }
  if (find_csv_value(raw, "Filter Change Time (seconds)", val))
    out.filter_change = to_int(val);
  if (find_csv_value(raw, "Brush Change Time (seconds)", val))
    out.brush_change = to_int(val);
  if (find_csv_value(raw, "Dirt Bin Alert Reminder Interval (minutes)", val))
    out.dirt_bin = to_int(val);
  return found;
}

// Ported from firmware/src/cleaning_history.cpp parsePose (MIT)
bool parse_pose(const std::string &raw, Pose &out) {
  size_t x_pos = raw.find("X=");
  size_t y_pos = raw.find("Y=");
  size_t t_pos = raw.find("Theta=");
  size_t tm_pos = raw.find("Time=");
  if (x_pos == std::string::npos || y_pos == std::string::npos || t_pos == std::string::npos ||
      tm_pos == std::string::npos)
    return false;
  out.x = strtof(raw.c_str() + x_pos + 2, nullptr);
  out.y = strtof(raw.c_str() + y_pos + 2, nullptr);
  out.theta = strtof(raw.c_str() + t_pos + 6, nullptr);
  out.time = strtof(raw.c_str() + tm_pos + 5, nullptr);
  return true;
}

bool parse_lds_scan(const std::string &raw, LdsScan &out) {
  out.valid_points = 0;
  out.rotation_speed = 0.0f;
  for (int i = 0; i < 360; i++) {
    out.dist[i] = 0;
    out.intensity[i] = 0;
    out.error[i] = 0;
  }
  bool found = false;
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t line_end = raw.find('\n', pos);
    if (line_end == std::string::npos)
      line_end = raw.size();
    std::string line = trim(raw.substr(pos, line_end - pos));
    pos = line_end + 1;
    if (line.empty() || line.compare(0, 8, "AngleDeg") == 0 || line.compare(0, 14, "AngleInDegrees") == 0)
      continue;
    if (line.compare(0, 14, "ROTATION_SPEED") == 0) {
      size_t comma = line.find(',');
      if (comma != std::string::npos)
        out.rotation_speed = to_float(trim(line.substr(comma + 1)));
      continue;
    }
    size_t c1 = line.find(',');
    if (c1 == std::string::npos)
      continue;
    size_t c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos)
      continue;
    size_t c3 = line.find(',', c2 + 1);
    if (c3 == std::string::npos)
      continue;
    int angle = to_int(line.substr(0, c1));
    if (angle < 0 || angle >= 360)
      continue;
    long dist = strtol(line.c_str() + c1 + 1, nullptr, 10);
    long intensity = strtol(line.c_str() + c2 + 1, nullptr, 10);
    long err = strtol(line.c_str() + c3 + 1, nullptr, 10);
    auto clamp16 = [](long v) -> uint16_t { return static_cast<uint16_t>(v < 0 ? 0 : (v > 65535 ? 65535 : v)); };
    out.dist[angle] = clamp16(dist);
    out.intensity[angle] = clamp16(intensity);
    out.error[angle] = clamp16(err);
    if (err == 0 && dist > 0)
      out.valid_points++;
    found = true;
  }
  return found;
}

std::string lds_array_json(const uint16_t *values) {
  std::string out;
  out.reserve(360 * 5 + 2);
  out += '[';
  char buf[8];
  for (int i = 0; i < 360; i++) {
    if (i > 0)
      out += ',';
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(values[i]));
    out += buf;
  }
  out += ']';
  return out;
}

std::string lds_scan_json(const LdsScan &scan) {
  std::string out = "{\"rotationSpeed\":" + format_float(scan.rotation_speed, 2) +
                    ",\"validPoints\":" + std::to_string(scan.valid_points) + ",\"dist\":";
  out += lds_array_json(scan.dist);
  out += ",\"intensity\":";
  out += lds_array_json(scan.intensity);
  out += ",\"error\":";
  out += lds_array_json(scan.error);
  out += '}';
  return out;
}

// -- Helpers -----------------------------------------------------------------

bool is_supported_model(const std::string &model_name) {
  if (model_name.size() < 8 || strncasecmp(model_name.c_str(), "botvacd", 7) != 0)
    return false;
  char digit = model_name[7];
  return digit >= '3' && digit <= '7';
}

std::string compute_skey(const std::string &serial_number) {
  size_t comma = serial_number.find(',');
  if (comma == std::string::npos || serial_number.size() < comma + 13)
    return "";
  std::string mac = serial_number.substr(comma + 1, 12);

  // RC4 key schedule with Neato's fixed seed
  static const uint8_t SEED[] = {0x68, 0x36, 0x43, 0x58, 0x09, 0x09, 0x3A, 0x3C, 0x2A, 0x7B, 0x59};
  uint8_t s[256];
  for (int i = 0; i < 256; i++)
    s[i] = static_cast<uint8_t>(i);
  int j = 0;
  for (int i = 0; i < 256; i++) {
    j = (j + s[i] + SEED[i % 11]) & 0xFF;
    uint8_t tmp = s[i];
    s[i] = s[j];
    s[j] = tmp;
  }
  // PRGA — 12 keystream bytes
  uint8_t ks[12];
  int ii = 0;
  j = 0;
  for (int k = 0; k < 12; k++) {
    ii = (ii + 1) & 0xFF;
    j = (j + s[ii]) & 0xFF;
    uint8_t tmp = s[ii];
    s[ii] = s[j];
    s[j] = tmp;
    ks[k] = s[(s[ii] + s[j]) & 0xFF];
  }
  char result[26];
  for (int k = 0; k < 12; k++)
    snprintf(&result[k * 2], 3, "%02x", static_cast<unsigned>(ks[k] ^ static_cast<uint8_t>(mac[k])));
  // Brainslug quirk: append result[len/2] at the end
  result[24] = result[6];
  result[25] = '\0';
  return result;
}

std::string build_set_event(const char *event, const std::string &skey) {
  return std::string("SetEvent event ") + event + " SKey " + skey;
}

bool is_idle_state(const std::string &ui) { return ui == "UIMGR_STATE_IDLE" || ui == "UIMGR_STATE_STANDBY"; }

bool is_cleaning_state(const std::string &ui) {
  return contains(ui, "HOUSECLEANINGRUNNING") || contains(ui, "HOUSECLEANINGPAUSED") ||
         contains(ui, "SPOTCLEANINGRUNNING") || contains(ui, "SPOTCLEANINGPAUSED");
}

bool is_paused_state(const std::string &ui) { return contains(ui, "CLEANINGPAUSED"); }
bool is_docking_state(const std::string &ui) { return contains(ui, "DOCKING"); }
bool is_suspended_state(const std::string &ui) { return contains(ui, "CLEANINGSUSPENDED"); }

bool is_active_state(const std::string &ui) {
  return contains(ui, "CLEANINGRUNNING") || contains(ui, "CLEANINGPAUSED") || contains(ui, "CLEANINGSUSPENDED") ||
         contains(ui, "DOCKING") || contains(ui, "MANUALCLEANING");
}

bool is_charging_mid_clean(const std::string &robot_state) { return contains(robot_state, "Charging_Cleaning"); }

const char *clean_mode_from_state(const std::string &ui) {
  if (contains(ui, "HOUSECLEANING"))
    return "house";
  if (contains(ui, "SPOTCLEANING"))
    return "spot";
  if (contains(ui, "MANUALCLEANING"))
    return "manual";
  return "unknown";
}

const char *compute_activity(const std::string &ui, bool manual_active, bool has_error, const std::string &kind,
                             bool charging, bool ext_power) {
  // Alerts (warnings) describe what the robot is doing; only real errors win.
  if (has_error && kind != "warning")
    return "error";
  if (manual_active || contains(ui, "MANUALCLEANING"))
    return "manual";
  if (contains(ui, "CLEANINGRUNNING"))
    return "cleaning";
  if (contains(ui, "CLEANINGPAUSED") || contains(ui, "CLEANINGSUSPENDED"))
    return "paused";
  if (contains(ui, "DOCKING"))
    return "returning";
  if (charging || ext_power)
    return "docked";
  return "idle";
}

std::string build_set_time(int year, int month, int day, int hour, int minute, int second) {
  char buf[64];
  snprintf(buf, sizeof(buf), "SetTime All %04d-%02d-%02d--%02d:%02d:%02d", year, month, day, hour, minute, second);
  return buf;
}

}  // namespace neatocore
