// StartGuard: persistent-map recovery and the mapping run.
#include "test_framework.h"

#include "../components/openneato/neato_mapping.h"

using neatocore::StartGuard;
using Action = StartGuard::Action;
using Kind = StartGuard::Kind;

static const char *STARTING = "UIMGR_STATE_STARTHOUSECLEANING";
static const char *RUN = "UIMGR_STATE_HOUSECLEANINGRUNNING";
static const char *IDLE = "UIMGR_STATE_IDLE";

TEST(guard_house_clean_that_starts_needs_nothing) {
  StartGuard g;
  CHECK(g.update(IDLE, false, 200, true, 0) == Action::NONE);
  g.begin(Kind::HOUSE, 0);
  CHECK(g.busy());
  CHECK(!g.mapping());
  CHECK(g.update(STARTING, true, 236, true, 3000) == Action::NONE);  // acquiring: wait
  CHECK(g.update(RUN, false, 200, true, 8000) == Action::DONE);
  CHECK(!g.busy());
  CHECK(!g.gave_up());
}

TEST(guard_recovers_with_a_robot_restart) {
  // What a Botvac D5 without the Neato cloud does (field log 2026-10-16).
  StartGuard g;
  g.begin(Kind::MAPPING, 0);
  CHECK(g.mapping());
  CHECK(g.update(STARTING, true, 236, true, 1000) == Action::NONE);
  CHECK(g.update(STARTING, true, 234, true, 11000) == Action::STOP);
  CHECK(g.update(STARTING, true, 234, true, 13000) == Action::NONE);
  CHECK(g.update(IDLE, false, 200, true, 25000) == Action::RESTART);
  // The robot drops off the serial link while it reboots, then comes back idle.
  CHECK(g.update(IDLE, false, 200, true, 27000) == Action::NONE);
  CHECK(g.update(IDLE, false, 200, false, 35000) == Action::NONE);
  CHECK(g.update(IDLE, false, 200, true, 70000) == Action::START);
  CHECK(g.mapping());
  CHECK(g.update(RUN, false, 200, true, 76000) == Action::NONE);
  CHECK(g.update("UIMGR_STATE_DOCKINGRUNNING", false, 200, true, 900000) == Action::NONE);
  CHECK(g.update(IDLE, false, 200, true, 1000000) == Action::DONE);
  CHECK(!g.gave_up());
  CHECK(!g.mapping());
}

TEST(guard_gives_up_after_a_second_refusal) {
  StartGuard g;
  g.begin(Kind::HOUSE, 0);
  CHECK(g.update(STARTING, true, 234, true, 10000) == Action::STOP);
  CHECK(g.update(IDLE, false, 200, true, 20000) == Action::RESTART);
  CHECK(g.update(IDLE, false, 200, false, 25000) == Action::NONE);
  CHECK(g.update(IDLE, false, 200, true, 60000) == Action::START);
  CHECK(g.update(STARTING, true, 234, true, 70000) == Action::STOP);
  CHECK(g.update(IDLE, false, 200, true, 80000) == Action::DONE);
  CHECK(g.gave_up());
  CHECK(!g.busy());
}

TEST(guard_retries_and_times_out) {
  StartGuard g;
  g.begin(Kind::SPOT, 0);
  // Start ignored: re-sent while the robot stays idle.
  CHECK(g.update(IDLE, false, 200, true, StartGuard::RETRY_MS) == Action::START);
  CHECK(g.update(IDLE, false, 200, true, StartGuard::START_TIMEOUT_MS + 1) == Action::DONE);
  CHECK(g.gave_up());

  g.begin(Kind::HOUSE, 0);
  CHECK(g.update(STARTING, true, 235, true, 1000) == Action::STOP);
  CHECK(g.update(STARTING, true, 235, true, 1000 + StartGuard::RETRY_MS) == Action::STOP);
  CHECK(g.update(STARTING, true, 235, true, 1000 + StartGuard::STOP_TIMEOUT_MS + 1) == Action::DONE);

  // Robot never answers again after the restart.
  g.begin(Kind::HOUSE, 0);
  CHECK(g.update(STARTING, true, 234, true, 1000) == Action::STOP);
  CHECK(g.update(IDLE, false, 200, true, 2000) == Action::RESTART);
  CHECK(g.update(IDLE, false, 200, false, 2000 + StartGuard::RESTART_TIMEOUT_MS + 1) == Action::DONE);
}

TEST(guard_ignores_other_errors) {
  StartGuard g;
  g.begin(Kind::HOUSE, 0);
  CHECK(g.update(STARTING, true, 222, true, 2000) == Action::NONE);  // bin full
  CHECK(g.busy());
  CHECK(!StartGuard::is_persistent_map_failure(236));
  CHECK(StartGuard::is_persistent_map_failure(234));
}
