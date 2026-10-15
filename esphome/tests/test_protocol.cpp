#include "../components/openneato/neato_protocol.h"
#include "test_framework.h"

#include <memory>

using namespace neatocore;

TEST(version_parses_model_serial_and_time) {
  const std::string raw =
      "GetVersion\r\nComponent,Major,Minor,Build,\r\nModel,BotVacD7Connected,905-0415,\r\n"
      "Serial Number,OPS01234-5678,0123456789AB,P,\r\nSoftware,4,6,0,\r\nLDS Software,V2.6.15295,0000000001,\r\n"
      "MainBoard Version,15,0,\r\nTime UTC,Sat Apr 11 19:26:13 2026\r\n";
  VersionData v;
  CHECK(parse_version(raw, v));
  CHECK_STR(v.model_name, "BotVacD7Connected");
  CHECK_STR(v.serial_number, "OPS01234-5678,0123456789AB,P");
  CHECK_STR(v.software_version, "4.6.0");
  CHECK_STR(v.lds_version, "V2.6.15295,0000000001");
  CHECK_STR(v.main_board_version, "15.0");
  CHECK_EQ(v.time_utc, static_cast<int64_t>(1775935573));
  CHECK(is_supported_model(v.model_name));
  CHECK(!is_supported_model("XV11"));
  CHECK(!is_supported_model("BotVacD8"));
}

TEST(version_numeric_model_prefix) {
  VersionData v;
  CHECK(parse_version("GetVersion\r\nModelID,0,XV11,\r\n", v));
  CHECK_STR(v.model_name, "XV11");
}

TEST(skey_matches_reference_rc4) {
  CHECK_STR(compute_skey("OPS01234-5678,0123456789AB,P"), "b20e9dfade296f1210da702bf");
  CHECK_STR(compute_skey("no-mac"), "");
  CHECK_STR(compute_skey("X,short"), "");
  CHECK_STR(build_set_event(NEATO_EVT_PAUSE, "abc"),
            "SetEvent event UIMGR_EVENT_SMARTAPP_PAUSE_CLEANING SKey abc");
}

TEST(charger_parse) {
  const std::string raw =
      "GetCharger\r\nLabel,Value\r\nFuelPercent,87\r\nBatteryOverTemp,0\r\nChargingActive,1\r\n"
      "BatteryFailure,0\r\nExtPwrPresent,1\r\nVBattV,16.25\r\nVExtV,23.10\r\nBattTempCAvg,31\r\n";
  ChargerData c;
  CHECK(parse_charger(raw, c));
  CHECK_EQ(c.fuel_percent, 87);
  CHECK(c.charging_active);
  CHECK(c.ext_pwr_present);
  CHECK(!c.battery_failure);
  CHECK_NEAR(c.v_batt, 16.25, 0.001);
  CHECK_NEAR(c.v_ext, 23.10, 0.001);
  CHECK_EQ(c.batt_temp_c, 31);
  ChargerData empty;
  CHECK(!parse_charger("GetCharger\r\n", empty));
}

TEST(analog_warranty_digital_motor_parse) {
  AnalogData a;
  CHECK(parse_analog("GetAnalogSensors\r\nSensorName,Unit,Value\r\nBatteryVoltage,mV,16250,\r\n"
                     "BatteryCurrent,mA,-512\r\nBatteryTemperature,mC,30500\r\nExternalVoltage,mV,0\r\n",
                     a));
  CHECK_NEAR(a.battery_voltage_v, 16.25, 0.001);
  CHECK_EQ(a.battery_current_ma, -512);
  CHECK_NEAR(a.battery_temperature_c, 30.5, 0.001);
  CHECK(a.has_current && a.has_temperature);

  WarrantyData w;
  CHECK(parse_warranty("GetWarranty\r\nCumulativeCleaningTimeInSecs,0001e240\r\nCumulativeBatteryCycles,0000002a\r\n", w));
  CHECK_EQ(w.cleaning_time_s, 123456);
  CHECK_EQ(w.battery_cycles, 42);

  DigitalSensorData d;
  CHECK(parse_digital_sensors("GetDigitalSensors\r\nDigital Sensor Name, Value\r\nSNSR_DC_JACK_IS_IN,0\r\n"
                              "SNSR_DUSTBIN_IS_IN,1\r\nSNSR_LEFT_WHEEL_EXTENDED,0\r\nSNSR_RIGHT_WHEEL_EXTENDED,1\r\n"
                              "LSIDEBIT,0\r\nLFRONTBIT,1\r\nLLDSBIT,0\r\nRSIDEBIT,1\r\nRFRONTBIT,0\r\nRLDSBIT,0\r\n",
                              d));
  CHECK(d.dustbin_in);
  CHECK(d.right_wheel_extended && !d.left_wheel_extended);
  CHECK(d.l_front && d.r_side && !d.l_side);

  MotorData m;
  CHECK(parse_motors("GetMotors\r\nParameter,Value\r\nBrush_RPM,1400\r\nVacuum_RPM,2900\r\nLeftWheel_Load%,35\r\n"
                     "RightWheel_Load%,81\r\nLaser_RPM,300\r\n",
                     m));
  CHECK_EQ(m.brush_rpm, 1400);
  CHECK_EQ(m.vacuum_rpm, 2900);
  CHECK_EQ(m.left_wheel_load, 35);
  CHECK_EQ(m.right_wheel_load, 81);
  CHECK_EQ(m.laser_rpm, 300);
}

TEST(state_parse) {
  RobotState s;
  CHECK(parse_state("GetState\r\nCurrent UI State is: UIMGR_STATE_HOUSECLEANINGRUNNING\r\n"
                    "Current Robot State is: ST_C_HouseCleaning\r\n",
                    s));
  CHECK_STR(s.ui_state, "UIMGR_STATE_HOUSECLEANINGRUNNING");
  CHECK_STR(s.robot_state, "ST_C_HouseCleaning");
  RobotState old;
  CHECK(parse_state("GetState\r\nCurrent UI State is: UIMGR_STATE_IDLE\r\n", old));
  CHECK_STR(old.robot_state, "");
}

TEST(error_parse) {
  ErrorData e;
  CHECK(parse_error("GetErr\r\n200 -  (UI_ALERT_INVALID)\r\n", e));
  CHECK(!e.has_error);
  CHECK_EQ(e.error_code, 200);
  CHECK(parse_error("GetErr\r\n200 -  (UI_ALERT_INVALID)\r\n243 - UI_ERROR_DUST_BIN_FULL\r\n", e));
  CHECK(e.has_error);
  CHECK_EQ(e.error_code, 243);
  CHECK_STR(e.kind, "error");
  CHECK_STR(e.display_message, "Dust bin is full");
  CHECK(parse_error("GetErr\r\n229 - UI_ALERT_BRUSH_CHANGE\r\n", e));
  CHECK_STR(e.kind, "warning");
  CHECK_STR(e.display_message, "Time to replace the brush");
  CHECK(parse_error("GetErr\r\n999 - SOMETHING\r\n", e));
  CHECK_STR(e.display_message, "Robot reported error 999");
  CHECK(parse_error("", e));
  CHECK(!e.has_error);
  CHECK_STR(e.kind, "");
}

TEST(user_settings_parse) {
  UserSettingsData u;
  CHECK(parse_user_settings("GetUserSettings\r\nClickSounds, OFF\r\nMelody Sounds, ON\r\nWarning Sounds, ON\r\n"
                            "Eco Mode, ON\r\nIntenseClean, OFF\r\nBin Full Detect, ON\r\nWall Enable, OFF\r\n"
                            "LED, OFF\r\nFilter Change Time (seconds), 43200\r\n"
                            "Brush Change Time (seconds), 86400\r\nDirt Bin Alert Reminder Interval (minutes), 45\r\n",
                            u));
  CHECK(!u.button_click);
  CHECK(u.eco_mode);
  CHECK(!u.wall_enable);
  CHECK(u.stealth_led);
  CHECK_EQ(u.filter_change, 43200);
  CHECK_EQ(u.brush_change, 86400);
  CHECK_EQ(u.dirt_bin, 45);
}

TEST(pose_parse) {
  Pose p;
  CHECK(parse_pose("GetRobotPos Smooth\r\nX=1.234 Y=-0.500 Theta=90.0 Time=1234.5\r\n", p));
  CHECK_NEAR(p.x, 1.234, 1e-4);
  CHECK_NEAR(p.y, -0.5, 1e-4);
  CHECK_NEAR(p.theta, 90.0, 1e-4);
  CHECK_NEAR(p.time, 1234.5, 1e-3);
  CHECK(!parse_pose("GetRobotPos Smooth\r\nnothing\r\n", p));
}

TEST(lds_scan_parse_and_json) {
  std::string raw = "GetLDSScan\r\nAngleInDegrees,DistInMM,Intensity,ErrorCodeHEX\r\n";
  for (int a = 0; a < 360; a++)
    raw += std::to_string(a) + "," + std::to_string(a == 5 ? 0 : 1000 + a) + "," + std::to_string(a % 7) + "," +
           (a == 7 ? "8035" : "0") + "\r\n";
  raw += "ROTATION_SPEED,5.12\r\n";
  std::unique_ptr<LdsScan> scan(new LdsScan());
  CHECK(parse_lds_scan(raw, *scan));
  CHECK_NEAR(scan->rotation_speed, 5.12, 1e-4);
  CHECK_EQ(scan->valid_points, 358);
  CHECK_EQ(static_cast<int>(scan->dist[359]), 1359);
  CHECK_EQ(static_cast<int>(scan->error[7]), 8035);
  std::string json = lds_scan_json(*scan);
  CHECK(json.find("{\"rotationSpeed\":5.12,\"validPoints\":358,\"dist\":[1000,1001,1002,1003,1004,0,") == 0);
  CHECK(json.find("\"intensity\":[0,1,2,3,4,5,6,0,") != std::string::npos);
  CHECK(json.find("\"error\":[0,0,0,0,0,0,0,8035,0") != std::string::npos);
  CHECK(json.back() == '}');
}

TEST(state_helpers_and_activity) {
  CHECK(is_idle_state("UIMGR_STATE_IDLE"));
  CHECK(is_idle_state("UIMGR_STATE_STANDBY"));
  CHECK(!is_idle_state("UIMGR_STATE_HOUSECLEANINGRUNNING"));
  CHECK(is_cleaning_state("UIMGR_STATE_SPOTCLEANINGPAUSED"));
  CHECK(!is_cleaning_state("UIMGR_STATE_MANUALCLEANING"));
  CHECK_STR(clean_mode_from_state("UIMGR_STATE_HOUSECLEANINGRUNNING"), "house");
  CHECK_STR(clean_mode_from_state("UIMGR_STATE_SPOTCLEANINGRUNNING"), "spot");

  CHECK_STR(compute_activity("UIMGR_STATE_HOUSECLEANINGRUNNING", false, false, "", false, false), "cleaning");
  CHECK_STR(compute_activity("UIMGR_STATE_HOUSECLEANINGRUNNING", false, true, "warning", false, false), "cleaning");
  CHECK_STR(compute_activity("UIMGR_STATE_HOUSECLEANINGRUNNING", false, true, "error", false, false), "error");
  CHECK_STR(compute_activity("UIMGR_STATE_HOUSECLEANINGPAUSED", false, false, "", false, false), "paused");
  CHECK_STR(compute_activity("UIMGR_STATE_HOUSECLEANINGSUSPENDED", false, false, "", true, true), "paused");
  CHECK_STR(compute_activity("UIMGR_STATE_DOCKING", false, false, "", false, false), "returning");
  CHECK_STR(compute_activity("UIMGR_STATE_STANDBY", false, false, "", true, true), "docked");
  CHECK_STR(compute_activity("UIMGR_STATE_IDLE", false, false, "", false, false), "idle");
  CHECK_STR(compute_activity("UIMGR_STATE_MANUALCLEANING", true, false, "", false, false), "manual");
}

TEST(set_time_command) {
  CHECK_STR(build_set_time(2026, 4, 1, 7, 5, 9), "SetTime All 2026-04-01--07:05:09");
}

TEST(sound_names) {
  CHECK_STR(SOUND_NAMES[0], "Waking up");
  CHECK_STR(SOUND_NAMES[SOUND_ALERT], "Alert");
  CHECK_STR(SOUND_NAMES[20], "Thank you");
}
