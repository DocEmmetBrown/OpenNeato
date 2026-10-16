// MappingRun: exploration with a quiet-house-clean fallback.
#include "test_framework.h"

#include "../components/openneato/neato_mapping.h"

using neatocore::MappingRun;
using Action = MappingRun::Action;

static const char *START = "UIMGR_STATE_STARTHOUSECLEANING";
static const char *RUN = "UIMGR_STATE_HOUSECLEANINGRUNNING";
static const char *IDLE = "UIMGR_STATE_IDLE";

TEST(mapping_falls_back_on_persistent_map_alert) {
  MappingRun m;
  CHECK(m.update(IDLE, false, 200, 0) == Action::NONE);
  m.start(1000);
  CHECK(m.busy());
  CHECK(m.update(START, false, 200, 3000) == Action::NONE);
  // The alert seen on a D5 without the Neato cloud
  CHECK(m.update(START, true, 234, 11000) == Action::FALLBACK);
  CHECK(m.overriding());
  CHECK(m.update(START, true, 234, 13000) == Action::NONE);  // reported once
  CHECK(m.update(RUN, false, 200, 20000) == Action::NONE);
  CHECK(m.overriding());
  CHECK(m.update("UIMGR_STATE_DOCKINGRUNNING", false, 200, 900000) == Action::NONE);
  CHECK(m.update(IDLE, false, 200, 1000000) == Action::RESTORE);
  CHECK(!m.busy());
  CHECK(!m.overriding());
}

TEST(mapping_exploration_that_runs_needs_nothing) {
  MappingRun m;
  m.start(0);
  CHECK(m.update(RUN, false, 200, 5000) == Action::NONE);
  CHECK(!m.busy());
  // A later alert belongs to the clean, not the start.
  CHECK(m.update(IDLE, true, 234, 6000) == Action::NONE);
}

TEST(mapping_timeouts) {
  MappingRun m;
  m.start(0);
  CHECK(m.update(START, false, 200, MappingRun::EXPLORE_TIMEOUT_MS + 1) == Action::NONE);
  CHECK(!m.busy());

  m.start(0);
  CHECK(m.update(START, true, 235, 1000) == Action::FALLBACK);
  // The fallback clean never starts: restore anyway.
  CHECK(m.update(IDLE, false, 200, 1000 + MappingRun::FALLBACK_START_TIMEOUT_MS + 1) == Action::RESTORE);
  CHECK(!m.busy());
}

TEST(mapping_other_errors_do_not_fall_back) {
  MappingRun m;
  m.start(0);
  CHECK(m.update(START, true, 222, 2000) == Action::NONE);  // bin full
  CHECK(m.busy());
  CHECK(!MappingRun::is_persistent_map_alert(200));
  CHECK(MappingRun::is_persistent_map_alert(236));
}
