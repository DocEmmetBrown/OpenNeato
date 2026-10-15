#include "../components/openneato/neato_history.h"
#include "test_framework.h"

using namespace neatocore;

namespace {

const char *IDLE = "UIMGR_STATE_IDLE";
const char *RUNNING = "UIMGR_STATE_HOUSECLEANINGRUNNING";
const char *PAUSED = "UIMGR_STATE_HOUSECLEANINGPAUSED";
const char *DOCKING = "UIMGR_STATE_DOCKING";
const char *SUSPENDED = "UIMGR_STATE_HOUSECLEANINGSUSPENDED";
const char *RECHARGE = "ST_M1_Charging_Cleaning";

const int64_t T0 = 1771683615;

// One poll cycle: state -> error -> pose (when the recorder asks for it).
void cycle(SessionRecorder &r, const char *ui, const char *rs, int64_t epoch, uint32_t ms, float x, float y,
           float t = 0.0f, int brush = 1400, bool err = false) {
  if (!r.on_state(ui, rs, 90, epoch, ms))
    return;
  if (!r.on_error(err))
    return;
  Pose p;
  p.x = x;
  p.y = y;
  p.theta = t;
  p.time = static_cast<float>(ms) / 1000.0f;
  r.on_pose(p, brush);
}

}  // namespace

TEST(ring_buffer_seq_and_wraparound) {
  PoseRing ring(4);
  CHECK_EQ(ring.first_seq(), 0u);
  bool more = true;
  CHECK(ring.since(0, 100, more).empty());
  CHECK(!more);
  for (int i = 0; i < 6; i++) {
    HistoryEntry e{};
    e.x = static_cast<float>(i);
    ring.push(e);
  }
  CHECK_EQ(ring.last_seq(), 6u);
  CHECK_EQ(ring.first_seq(), 3u);
  CHECK_EQ(ring.size(), static_cast<size_t>(4));
  auto all = ring.since(0, 100, more);
  CHECK_EQ(all.size(), static_cast<size_t>(4));
  CHECK_EQ(all.front()->seq, 3u);
  CHECK_EQ(all.back()->seq, 6u);
  CHECK(!more);
  auto two = ring.since(3, 2, more);
  CHECK_EQ(two.size(), static_cast<size_t>(2));
  CHECK_EQ(two[0]->seq, 4u);
  CHECK(more);
  CHECK(ring.since(6, 100, more).empty());
  CHECK(ring.since(99, 100, more).empty());
}

TEST(entry_json_format) {
  HistoryEntry e{};
  e.seq = 1233;
  e.sid = 1771683615;
  e.x = 1.234f;
  e.y = -0.5f;
  e.theta_ddeg = 900;
  e.ts = 1234.5f;
  e.brush_rpm = 1400;
  e.type = EntryType::POSE;
  CHECK_STR(entry_json(e), "{\"seq\":1233,\"sid\":1771683615,\"x\":1.234,\"y\":-0.500,\"t\":90.0,\"ts\":1234.5,\"b\":1400}");
  e.brush_rpm = -1;
  CHECK_STR(entry_json(e), "{\"seq\":1233,\"sid\":1771683615,\"x\":1.234,\"y\":-0.500,\"t\":90.0,\"ts\":1234.5}");
  e.type = EntryType::RECHARGE;
  e.seq = 1234;
  CHECK_STR(entry_json(e), "{\"seq\":1234,\"sid\":1771683615,\"type\":\"recharge\",\"x\":1.234,\"y\":-0.500}");
}

TEST(session_lifecycle_with_recharge_and_summary) {
  SessionRecorder r(256);
  cycle(r, IDLE, "", T0 - 10, 0, 0, 0);
  CHECK(!r.collecting());
  CHECK_STR(r.current_json(), "null");
  CHECK_STR(r.previous_json(), "null");

  cycle(r, RUNNING, "", T0, 2000, 0.0f, 0.0f, 0.0f);
  CHECK(r.collecting());
  CHECK(r.active());
  CHECK_STR(r.current_json(),
            "{\"id\":1771683615,\"active\":true,\"header\":{\"type\":\"session\",\"mode\":\"house\",\"time\":1771683615,"
            "\"battery\":90}}");
  cycle(r, RUNNING, "", T0 + 2, 4000, 3.0f, 0.0f, 90.0f);
  cycle(r, RUNNING, "", T0 + 4, 6000, 3.0f, 4.0f, 180.0f, -1, true);
  CHECK_EQ(r.snapshots(), static_cast<size_t>(3));
  CHECK_NEAR(r.distance_m(), 7.0, 1e-4);

  // Paused: no poses, session inactive.
  cycle(r, PAUSED, "", T0 + 6, 8000, 9, 9);
  CHECK(!r.active());
  CHECK_EQ(r.snapshots(), static_cast<size_t>(3));

  // Mid-clean recharge: one marker at the last pose, no poses while docked.
  cycle(r, DOCKING, RECHARGE, T0 + 8, 10000, 9, 9);
  cycle(r, SUSPENDED, RECHARGE, T0 + 10, 12000, 9, 9);
  CHECK(r.collecting());
  CHECK(!r.active());
  CHECK_EQ(r.recharges(), 1);
  cycle(r, RUNNING, "ST_C_HouseCleaning", T0 + 12, 14000, 3.0f, 4.0f, 180.0f);
  CHECK(r.active());
  // Origin reset right before the end is dropped (robot was 5 m away).
  cycle(r, RUNNING, "", T0 + 14, 16000, 0.0f, 0.0f, 0.0f);
  CHECK_EQ(r.snapshots(), static_cast<size_t>(4));

  CHECK(!r.on_state(IDLE, "", 41, T0 + 100, 102000));
  CHECK(!r.collecting());
  CHECK_EQ(r.duration_s(T0 + 500, 500000), 100u);
  CHECK_STR(r.current_json(), "null");
  CHECK_STR(r.previous_json(),
            "{\"id\":1771683615,\"header\":{\"type\":\"session\",\"mode\":\"house\",\"time\":1771683615,\"battery\":90},"
            "\"summary\":{\"type\":\"summary\",\"time\":1771683715,\"duration\":100,\"mode\":\"house\",\"recharges\":1,"
            "\"snapshots\":4,\"distanceTraveled\":7.00,\"maxDistFromOrigin\":5.00,\"totalRotation\":180.0,"
            "\"areaCovered\":0.75,\"errorsDuringClean\":1,\"batteryStart\":90,\"batteryEnd\":41}}");

  bool more = true;
  std::string entries = r.entries_json(0, more);
  CHECK(!more);
  CHECK(entries.find("{\"seq\":3,\"sid\":1771683615,\"x\":3.000,\"y\":4.000,\"t\":180.0,\"ts\":6.0}") !=
        std::string::npos);
  CHECK(entries.find("{\"seq\":4,\"sid\":1771683615,\"type\":\"recharge\",\"x\":3.000,\"y\":4.000}") !=
        std::string::npos);
  CHECK_EQ(r.ring().last_seq(), 5u);

  // Buffer survives the session end; batch envelope.
  std::string batch = r.batch_json(3, 3735928559u);
  CHECK(batch.find("{\"boot\":3735928559,\"seq\":5,\"first_seq\":1,\"more\":false,\"current\":null,\"previous\":{") ==
        0);
  CHECK(batch.find("\"entries\":[{\"seq\":4,") != std::string::npos);
}

TEST(session_short_is_discarded_and_seq_continues) {
  SessionRecorder r(256);
  cycle(r, IDLE, "", 0, 0, 0, 0);
  cycle(r, "UIMGR_STATE_SPOTCLEANINGRUNNING", "", 0, 1000, 1, 1);
  CHECK(r.collecting());
  CHECK(r.current_json().find("\"id\":0,") != std::string::npos);
  CHECK(r.current_json().find("\"mode\":\"spot\"") != std::string::npos);
  CHECK(r.current_json().find("\"time\"") == std::string::npos);  // clock invalid
  cycle(r, IDLE, "", 0, 3000, 0, 0);
  CHECK(!r.collecting());
  CHECK_STR(r.previous_json(), "null");
  CHECK_EQ(r.ring().last_seq(), 1u);
  CHECK_EQ(r.duration_s(0, 99999), 2u);  // millis fallback
}

TEST(session_starts_on_mid_clean_recharge_after_boot) {
  SessionRecorder r(16);
  // Bridge boots while the robot is recharging mid-clean.
  CHECK(!r.on_state(SUSPENDED, RECHARGE, 50, T0, 0));
  CHECK(r.collecting());
  CHECK(!r.active());
  CHECK_EQ(r.recharges(), 1);
  CHECK_EQ(r.ring().last_seq(), 0u);  // no pose yet -> no marker
}

TEST(session_does_not_restart_while_cleaning_state_persists) {
  SessionRecorder r(16);
  cycle(r, RUNNING, "", T0, 0, 0, 0);  // first poll already cleaning -> starts
  CHECK(r.collecting());
  for (int i = 1; i <= 3; i++)
    cycle(r, RUNNING, "", T0 + i, i * 2000, static_cast<float>(i), 0);
  cycle(r, IDLE, "", T0 + 10, 10000, 0, 0);
  CHECK(!r.collecting());
  CHECK(r.previous_json() != "null");
  // Manual clean is not a recorded session.
  cycle(r, "UIMGR_STATE_MANUALCLEANING", "", T0 + 20, 20000, 0, 0);
  CHECK(!r.collecting());
}

TEST(batch_limits_to_100) {
  SessionRecorder r(300);
  cycle(r, IDLE, "", T0, 0, 0, 0);
  for (int i = 0; i < 250; i++)
    cycle(r, RUNNING, "", T0 + i, static_cast<uint32_t>(i) * 2000, static_cast<float>(i) * 0.1f, 0);
  bool more = false;
  std::string e = r.entries_json(0, more);
  CHECK(more);
  size_t count = 0;
  for (size_t p = e.find("\"seq\":"); p != std::string::npos; p = e.find("\"seq\":", p + 1))
    count++;
  CHECK_EQ(count, static_cast<size_t>(100));
  r.entries_json(200, more);
  CHECK(!more);
}
