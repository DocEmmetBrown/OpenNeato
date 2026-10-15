// Ported from OpenNeato firmware/src/neato_commands.h (MIT)
//
// Pure C++17 (no ESPHome / Arduino dependency): robot serial command constants,
// response parsers, the SetEvent SKey computation and UI-state helpers.
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

namespace neatocore {

// -- Commands ----------------------------------------------------------------

#define NEATO_CMD_GET_VERSION "GetVersion"
#define NEATO_CMD_GET_CHARGER "GetCharger"
#define NEATO_CMD_GET_ANALOG_SENSORS "GetAnalogSensors"
#define NEATO_CMD_GET_DIGITAL_SENSORS "GetDigitalSensors"
#define NEATO_CMD_GET_MOTORS "GetMotors"
#define NEATO_CMD_GET_STATE "GetState"
#define NEATO_CMD_GET_ERR "GetErr"
#define NEATO_CMD_GET_LDS_SCAN "GetLDSScan"
#define NEATO_CMD_GET_WARRANTY "GetWarranty"
#define NEATO_CMD_GET_ROBOT_POS_SMOOTH "GetRobotPos Smooth"
#define NEATO_CMD_GET_USER_SETTINGS "GetUserSettings"
#define NEATO_CMD_SET_USER_SETTINGS "SetUserSettings"
#define NEATO_CMD_SET_NAVIGATION_MODE "SetNavigationMode"
#define NEATO_CMD_TEST_MODE_ON "TestMode On"
#define NEATO_CMD_TEST_MODE_OFF "TestMode Off"
#define NEATO_CMD_SET_LDS_ROTATION_ON "SetLDSRotation On"
#define NEATO_CMD_SET_LDS_ROTATION_OFF "SetLDSRotation Off"
#define NEATO_CMD_PLAY_SOUND "PlaySound SoundID "
#define NEATO_CMD_SET_MOTOR "SetMotor"
#define NEATO_CMD_POWER_CYCLE "SetSystemMode PowerCycle"
#define NEATO_CMD_SHUTDOWN "SetSystemMode Shutdown"
#define NEATO_CMD_CLEAR_ERRORS "SetUIError clearall"
#define NEATO_CMD_NEW_BATTERY "NewBattery"

#define NEATO_EVT_START_HOUSE "UIMGR_EVENT_SMARTAPP_START_HOUSE_CLEANING"
#define NEATO_EVT_START_SPOT "UIMGR_EVENT_SMARTAPP_START_SPOT_CLEANING"
#define NEATO_EVT_PAUSE "UIMGR_EVENT_SMARTAPP_PAUSE_CLEANING"
#define NEATO_EVT_RESUME "UIMGR_EVENT_SMARTAPP_RESUME_CLEANING"
#define NEATO_EVT_STOP "UIMGR_EVENT_SMARTAPP_STOP_CLEANING"
#define NEATO_EVT_SEND_TO_BASE "UIMGR_EVENT_SMARTAPP_SEND_TO_BASE"

static const int SOUND_COUNT = 21;
static const int SOUND_ALERT = 19;
// Option labels of the `Sound` select, indexed by PlaySound SoundID.
extern const char *const SOUND_NAMES[SOUND_COUNT];

// -- Response structs --------------------------------------------------------

struct VersionData {
  std::string model_name;
  std::string serial_number;
  std::string software_version;  // "Major.Minor.Build"
  std::string lds_version;
  std::string main_board_version;
  int64_t time_utc = 0;  // Parsed from "Time UTC" (0 = not available)
};

struct ChargerData {
  int fuel_percent = -1;
  bool battery_over_temp = false;
  bool charging_active = false;
  bool battery_failure = false;
  bool ext_pwr_present = false;
  float v_batt = 0.0f;
  float v_ext = 0.0f;
  int batt_temp_c = -1;  // -1 = not available
};

struct AnalogData {
  float battery_voltage_v = 0.0f;
  int battery_current_ma = 0;
  float battery_temperature_c = 0.0f;
  float external_voltage_v = 0.0f;
  bool has_current = false;
  bool has_temperature = false;
};

struct WarrantyData {
  int cleaning_time_s = 0;
  int battery_cycles = 0;
};

struct DigitalSensorData {
  bool dc_jack_in = false;
  bool dustbin_in = false;
  bool left_wheel_extended = false;
  bool right_wheel_extended = false;
  bool l_side = false;
  bool l_front = false;
  bool l_lds = false;
  bool r_side = false;
  bool r_front = false;
  bool r_lds = false;
};

struct MotorData {
  int brush_rpm = 0;
  int vacuum_rpm = 0;
  int left_wheel_load = 0;
  int right_wheel_load = 0;
  int laser_rpm = 0;
};

struct RobotState {
  std::string ui_state;     // e.g. "UIMGR_STATE_STANDBY"
  std::string robot_state;  // e.g. "ST_C_Standby" (empty on old firmware)
};

struct ErrorData {
  bool has_error = false;
  std::string kind;  // "error" / "warning" / ""
  int error_code = 200;
  std::string display_message;
};

struct UserSettingsData {
  bool button_click = true;
  bool melodies = true;
  bool warnings = true;
  bool eco_mode = false;
  bool intense_clean = false;
  bool bin_full_detect = true;
  bool wall_enable = true;
  bool stealth_led = false;
  int filter_change = 2592000;  // seconds
  int brush_change = 2592000;   // seconds
  int dirt_bin = 30;            // minutes
};

struct Pose {
  float x = 0.0f;
  float y = 0.0f;
  float theta = 0.0f;
  float time = 0.0f;
};

struct LdsScan {
  float rotation_speed = 0.0f;
  int valid_points = 0;
  uint16_t dist[360];
  uint16_t intensity[360];
  uint16_t error[360];
};

// -- Parsers -----------------------------------------------------------------

bool parse_version(const std::string &raw, VersionData &out);
bool parse_charger(const std::string &raw, ChargerData &out);
bool parse_analog(const std::string &raw, AnalogData &out);
bool parse_warranty(const std::string &raw, WarrantyData &out);
bool parse_digital_sensors(const std::string &raw, DigitalSensorData &out);
bool parse_motors(const std::string &raw, MotorData &out);
bool parse_state(const std::string &raw, RobotState &out);
bool parse_error(const std::string &raw, ErrorData &out);
bool parse_user_settings(const std::string &raw, UserSettingsData &out);
bool parse_pose(const std::string &raw, Pose &out);
bool parse_lds_scan(const std::string &raw, LdsScan &out);

// Compact JSON for the `get_lidar` action:
// {"rotationSpeed":f,"validPoints":n,"dist":[..360],"intensity":[..360],"error":[..360]}
std::string lds_scan_json(const LdsScan &scan);
// Just one of the three 360-int arrays, as a JSON array literal.
std::string lds_array_json(const uint16_t *values);

// -- Helpers -----------------------------------------------------------------

bool is_supported_model(const std::string &model_name);

// SetEvent security key from the robot serial number ("XXX,<12 char MAC>,X").
// Returns "" when the serial number has no MAC part.
std::string compute_skey(const std::string &serial_number);

std::string build_set_event(const char *event, const std::string &skey);

bool contains(const std::string &haystack, const char *needle);
bool is_idle_state(const std::string &ui);  // IDLE or STANDBY
bool is_cleaning_state(const std::string &ui);  // house/spot running or paused (not manual)
bool is_paused_state(const std::string &ui);
bool is_docking_state(const std::string &ui);
bool is_suspended_state(const std::string &ui);
bool is_active_state(const std::string &ui);  // running/paused/suspended/docking/manual
bool is_charging_mid_clean(const std::string &robot_state);
const char *clean_mode_from_state(const std::string &ui);

// `activity` text sensor: cleaning, paused, returning, docked, idle, error, manual.
const char *compute_activity(const std::string &ui, bool manual_active, bool has_error, const std::string &kind,
                             bool charging, bool ext_power);

// "SetTime All YYYY-MM-DD--hh:mm:ss" from broken-down time fields.
std::string build_set_time(int year, int month, int day, int hour, int minute, int second);

// Small string utilities shared by the modules.
std::string trim(const std::string &s);
std::string format_float(float value, int decimals);

}  // namespace neatocore
