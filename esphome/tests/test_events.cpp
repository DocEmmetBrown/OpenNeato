#include "../components/openneato/neato_events.h"
#include "test_framework.h"

using namespace neatocore;

namespace {

std::string join(const std::vector<const char *> &events) {
  std::string out;
  for (auto *e : events) {
    if (!out.empty())
      out += ",";
    out += e;
  }
  return out;
}

const char *IDLE = "UIMGR_STATE_IDLE";
const char *STANDBY = "UIMGR_STATE_STANDBY";
const char *RUNNING = "UIMGR_STATE_HOUSECLEANINGRUNNING";
const char *PAUSED = "UIMGR_STATE_HOUSECLEANINGPAUSED";
const char *DOCKING = "UIMGR_STATE_DOCKING";
const char *SUSPENDED = "UIMGR_STATE_HOUSECLEANINGSUSPENDED";
const char *RECHARGE = "ST_M1_Charging_Cleaning";

}  // namespace

TEST(events_first_snapshot_only_seeds) {
  EventTracker t;
  CHECK_STR(join(t.update_state(RUNNING, "")), "");
  CHECK_STR(join(t.update_state(RUNNING, "")), "");
}

TEST(events_cleaning_started_only_from_idle) {
  EventTracker t;
  t.update_state(IDLE, "");
  CHECK_STR(join(t.update_state(RUNNING, "")), "cleaning_started");
  // Resume from pause is not a new start.
  CHECK_STR(join(t.update_state(PAUSED, "")), "");
  CHECK_STR(join(t.update_state(RUNNING, "")), "");
  // Resume after a docking run is not a start either.
  EventTracker t2;
  t2.update_state(DOCKING, "");
  CHECK_STR(join(t2.update_state(RUNNING, "")), "");
  // Spot clean from standby.
  EventTracker t3;
  t3.update_state(STANDBY, "");
  CHECK_STR(join(t3.update_state("UIMGR_STATE_SPOTCLEANINGRUNNING", "")), "cleaning_started");
}

TEST(events_clean_ends_in_place) {
  EventTracker t;
  t.update_state(IDLE, "");
  t.update_state(RUNNING, "");
  CHECK_STR(join(t.update_state(IDLE, "")), "cleaning_done");
}

TEST(events_clean_ends_on_dock_fires_done_and_returned) {
  EventTracker t;
  t.update_state(IDLE, "");
  t.update_state(RUNNING, "");
  CHECK_STR(join(t.update_state(DOCKING, "ST_C_Docking")), "");
  CHECK(t.cleaning_before_dock());
  CHECK_STR(join(t.update_state(STANDBY, "ST_C_Standby")), "cleaning_done,returned_to_base");
  CHECK(!t.cleaning_before_dock());
}

TEST(events_mid_clean_recharge_cycle) {
  EventTracker t;
  t.update_state(IDLE, "");
  t.update_state(RUNNING, "");
  CHECK_STR(join(t.update_state(DOCKING, RECHARGE)), "docking_recharge");
  // DOCKING -> CLEANINGSUSPENDED keeps the cleaning context.
  CHECK_STR(join(t.update_state(SUSPENDED, RECHARGE)), "");
  CHECK(t.cleaning_before_dock());
  // Resumes cleaning: no new start (prev contained CLEANING).
  CHECK_STR(join(t.update_state(RUNNING, "ST_C_HouseCleaning")), "");
  CHECK_STR(join(t.update_state(IDLE, "")), "cleaning_done");
}

TEST(events_suspended_then_idle_is_done) {
  EventTracker t;
  t.update_state(IDLE, "");
  t.update_state(RUNNING, "");
  t.update_state(DOCKING, RECHARGE);
  t.update_state(SUSPENDED, RECHARGE);
  // Clean cancelled while recharging on the dock.
  CHECK_STR(join(t.update_state(IDLE, "")), "cleaning_done");
}

TEST(events_docking_while_recharging_to_idle_not_returned) {
  EventTracker t;
  t.update_state(IDLE, "");
  t.update_state(RUNNING, "");
  t.update_state(DOCKING, RECHARGE);
  // Not "done" (still recharging), not "returned" (recharging).
  CHECK_STR(join(t.update_state(IDLE, RECHARGE)), "");
}

TEST(events_plain_return_to_base) {
  EventTracker t;
  t.update_state(IDLE, "");
  // Send-to-base from idle: not a clean.
  CHECK_STR(join(t.update_state(DOCKING, "")), "");
  CHECK_STR(join(t.update_state(STANDBY, "")), "returned_to_base");
}

TEST(events_leaving_docking_clears_context) {
  EventTracker t;
  t.update_state(IDLE, "");
  t.update_state(RUNNING, "");
  t.update_state(DOCKING, "");
  // Docking -> paused (user paused) clears the flag.
  t.update_state(PAUSED, "");
  CHECK(!t.cleaning_before_dock());
}

TEST(events_errors_and_alerts) {
  EventTracker t;
  // Present at boot: seeds only.
  CHECK_STR(join(t.update_error(true, 243, "error")), "");
  CHECK_STR(join(t.update_error(true, 243, "error")), "");
  // Code change.
  CHECK_STR(join(t.update_error(true, 229, "warning")), "alert");
  CHECK_STR(join(t.update_error(false, 200, "")), "");
  CHECK_STR(join(t.update_error(true, 243, "error")), "error");
  CHECK_STR(join(t.update_error(true, 243, "error")), "");

  EventTracker fresh;
  CHECK_STR(join(fresh.update_error(false, 200, "")), "");
  CHECK_STR(join(fresh.update_error(true, 201, "warning")), "alert");
}
