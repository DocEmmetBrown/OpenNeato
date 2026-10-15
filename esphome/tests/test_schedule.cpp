#include "../components/openneato/neato_schedule.h"
#include "test_framework.h"

using namespace neatocore;

TEST(schedule_day_conversion) {
  CHECK_EQ(to_schedule_day(0), 6);  // Sunday
  CHECK_EQ(to_schedule_day(1), 0);  // Monday
  CHECK_EQ(to_schedule_day(6), 5);  // Saturday
  CHECK_STR(SCHEDULE_DAY_KEYS[0], "monday");
  CHECK_STR(SCHEDULE_DAY_NAMES[6], "Sunday");
}

TEST(schedule_due_window) {
  int mins = 0;
  CHECK(is_action_due(9, 0, 9 * 60, -1, mins));
  CHECK_EQ(mins, 540);
  CHECK(is_action_due(9, 0, 9 * 60 + 5, -1, mins));
  CHECK(!is_action_due(9, 0, 9 * 60 + 6, -1, mins));
  CHECK(!is_action_due(9, 0, 9 * 60 - 1, -1, mins));
  CHECK(!is_action_due(9, 0, 9 * 60 + 1, 540, mins));  // already fired
}

TEST(schedule_fired_guard_resets_on_new_day) {
  FiredGuard g;
  g.reset_for_day(2);
  g.claim(0, 540);
  g.reset_for_day(2);
  CHECK_EQ(g.fired(0), 540);
  g.reset_for_day(3);
  CHECK_EQ(g.fired(0), -1);
  g.claim(1, 600);
  g.release(1);
  CHECK_EQ(g.fired(1), -1);
}

TEST(schedule_next_slot) {
  Schedule s;
  CHECK(!next_slot(s, 0, 0).found);  // disabled
  s.enabled = true;
  CHECK(!next_slot(s, 0, 0).found);  // nothing enabled
  s.slots[0][0] = {true, 9, 0};    // Mon 09:00
  s.slots[0][1] = {true, 8, 30};   // Mon 08:30 (slot 2 earlier than slot 1)
  s.slots[3][0] = {true, 18, 15};  // Thu 18:15

  NextSlot n = next_slot(s, 0, 8 * 60);  // Monday 08:00
  CHECK(n.found);
  CHECK_EQ(n.day, 0);
  CHECK_EQ(n.slot, 1);
  CHECK_EQ(n.minutes, 510);
  CHECK_EQ(n.day_offset, 0);

  n = next_slot(s, 0, 8 * 60 + 30);  // same minute counts
  CHECK_EQ(n.slot, 1);

  n = next_slot(s, 0, 8 * 60 + 31);
  CHECK_EQ(n.slot, 0);
  CHECK_EQ(n.minutes, 540);

  n = next_slot(s, 0, 10 * 60);  // after both -> Thursday
  CHECK_EQ(n.day, 3);
  CHECK_EQ(n.day_offset, 3);
  CHECK_EQ(n.minutes, 18 * 60 + 15);

  n = next_slot(s, 4, 0);  // Friday -> next Monday 08:30
  CHECK_EQ(n.day, 0);
  CHECK_EQ(n.day_offset, 3);
  CHECK_EQ(n.slot, 1);

  // Skip-next: Monday 08:00 skips 08:30 and lands on 09:00.
  n = next_slot(s, 0, 8 * 60, 1);
  CHECK_EQ(n.slot, 0);
  CHECK_EQ(n.minutes, 540);

  // Only one slot all week: same weekday next week (and skip -> two weeks).
  Schedule one;
  one.enabled = true;
  one.slots[2][0] = {true, 7, 0};
  n = next_slot(one, 2, 8 * 60);
  CHECK(n.found);
  CHECK_EQ(n.day_offset, 7);
  n = next_slot(one, 2, 6 * 60, 1);
  CHECK_EQ(n.day_offset, 7);
}
