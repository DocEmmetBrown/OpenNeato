// MappingRun: exploration with a quiet-house-clean fallback.
#include "test_framework.h"

#include "../components/openneato/neato_mapping.h"

using neatocore::MappingRun;
using Action = MappingRun::Action;

static const char *START = "UIMGR_STATE_STARTHOUSECLEANING";
static const char *RUN = "UIMGR_STATE_HOUSECLEANINGRUNNING";
static const char *IDLE = "UIMGR_STATE_IDLE";

TEST(mapping_waits_for_failure_then_idle) {
  MappingRun m;
  CHECK(m.update(IDLE, false, 200, 0) == Action::NONE);
  m.start(1000);
  CHECK(m.busy());
  // 236 "Acquiring map data" is the attempt in progress: wait.
  CHECK(m.update(START, true, 236, 11000) == Action::NONE);
  CHECK(m.update(START, true, 234, 21000) == Action::STOP);
  CHECK(!m.overriding());
  CHECK(m.update(START, true, 234, 23000) == Action::NONE);  // still stopping
  CHECK(m.update(IDLE, false, 200, 35000) == Action::START);
  CHECK(m.overriding());
  CHECK(m.update(IDLE, false, 200, 37000) == Action::NONE);  // starting
  CHECK(m.update(RUN, false, 200, 45000) == Action::NONE);
  CHECK(m.update("UIMGR_STATE_DOCKINGRUNNING", false, 200, 900000) == Action::NONE);
  CHECK(m.update(IDLE, false, 200, 1000000) == Action::RESTORE);
  CHECK(!m.busy());
  CHECK(!m.overriding());
}

TEST(mapping_retries_stop_and_start) {
  MappingRun m;
  m.start(0);
  CHECK(m.update(START, true, 234, 1000) == Action::STOP);
  CHECK(m.update(START, true, 234, 1000 + MappingRun::RETRY_MS) == Action::STOP);
  CHECK(m.update(IDLE, false, 200, 40000) == Action::START);
  CHECK(m.update(IDLE, false, 200, 40000 + MappingRun::RETRY_MS) == Action::START);
  CHECK(m.update(START, false, 200, 80000) == Action::NONE);  // accepted, getting going
  CHECK(m.update(IDLE, false, 200, 40000 + MappingRun::START_TIMEOUT_MS + 1) == Action::RESTORE);
}

TEST(mapping_exploration_that_runs_needs_nothing) {
  MappingRun m;
  m.start(0);
  CHECK(m.update(RUN, false, 200, 5000) == Action::NONE);
  CHECK(!m.busy());
  CHECK(m.update(IDLE, true, 234, 6000) == Action::NONE);
}

TEST(mapping_timeouts) {
  MappingRun m;
  m.start(0);
  CHECK(m.update(START, false, 200, MappingRun::EXPLORE_TIMEOUT_MS + 1) == Action::NONE);
  CHECK(!m.busy());

  m.start(0);
  CHECK(m.update(START, true, 235, 1000) == Action::STOP);
  // Never leaves STARTHOUSECLEANING: give up without touching Eco.
  CHECK(m.update(START, true, 235, 1000 + MappingRun::STOP_TIMEOUT_MS + 1) == Action::NONE);
  CHECK(!m.busy());
}

TEST(mapping_other_errors_do_not_fall_back) {
  MappingRun m;
  m.start(0);
  CHECK(m.update(START, true, 222, 2000) == Action::NONE);  // bin full
  CHECK(m.busy());
  CHECK(!MappingRun::is_persistent_map_failure(236));
  CHECK(MappingRun::is_persistent_map_failure(234));
}
