#include "../components/openneato/neato_manual.h"
#include "test_framework.h"

using namespace neatocore;

TEST(manual_move_rules) {
  SafetyState s;
  CHECK(is_move_allowed(s, 100, 100));
  s.wheel_lifted = true;
  CHECK(!is_move_allowed(s, -100, -100));
  s = SafetyState{};
  s.bumper_front_left = true;
  CHECK(!is_move_allowed(s, 100, 100));  // forward
  CHECK(!is_move_allowed(s, 100, -100));  // turning right into the hit side
  CHECK(is_move_allowed(s, -100, -100));  // reverse
  CHECK(!is_move_allowed(s, -100, 100));  // pivot still drives one wheel forward
  s = SafetyState{};
  s.bumper_side_right = true;
  CHECK(!is_move_allowed(s, 0, 100));  // left turn into the right side
  CHECK(is_move_allowed(s, 100, 0));
}

TEST(manual_stall_detection_and_escape) {
  SafetyState s;
  int count = 0;
  for (int i = 0; i < MANUAL_STALL_COUNT - 1; i++)
    CHECK(!stall_step(s, count, 100, 100, 90, 10, 80));
  CHECK(!stall_step(s, count, 100, 100, 10, 10, 80));  // load drops -> reset
  CHECK_EQ(count, 0);
  for (int i = 0; i < MANUAL_STALL_COUNT - 1; i++)
    stall_step(s, count, 100, 100, 85, 85, 80);
  CHECK(stall_step(s, count, 100, 100, 85, 85, 80));
  CHECK(s.stall_front);
  CHECK(!s.stall_rear);
  CHECK(!is_move_allowed(s, 50, 50));
  CHECK(is_move_allowed(s, -50, -50));  // escape clears the front stall
  CHECK(!s.stall_front);
  CHECK(is_move_allowed(s, 50, 50));
  // Load on a wheel that was not commanded does not count.
  count = 0;
  for (int i = 0; i < 10; i++)
    CHECK(!stall_step(s, count, 0, 100, 99, 10, 80));
}
