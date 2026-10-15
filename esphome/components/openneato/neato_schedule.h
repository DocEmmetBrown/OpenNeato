// Ported from OpenNeato firmware/src/scheduler.{h,cpp} (MIT)
//
// Pure schedule arithmetic: 7 days x 2 slots (Mon=0..Sun=6), due-window check,
// per-day fired guards and next-slot lookup. The ESPHome hub drives it.
#pragma once

#include <cstdint>

namespace neatocore {

static const int SCHEDULE_DAYS = 7;
static const int SCHEDULE_SLOTS_PER_DAY = 2;
static const int SCHEDULE_WINDOW_MINS = 5;  // fire 0..N minutes after the slot time
static const uint32_t SCHEDULE_CHECK_INTERVAL_MS = 30000;
static const uint32_t RESTART_BOOT_TIMEOUT_MS = 120000;

extern const char *const SCHEDULE_DAY_KEYS[SCHEDULE_DAYS];   // "monday".."sunday"
extern const char *const SCHEDULE_DAY_NAMES[SCHEDULE_DAYS];  // "Monday".."Sunday"

struct ScheduleSlot {
  bool on = false;
  uint8_t hour = 9;
  uint8_t minute = 0;
};

struct Schedule {
  bool enabled = false;
  ScheduleSlot slots[SCHEDULE_DAYS][SCHEDULE_SLOTS_PER_DAY];
};

struct NextSlot {
  bool found = false;
  int day = 0;         // schedule day (Mon=0)
  int slot = 0;        // slot index
  int day_offset = 0;  // 0 = today .. 7 = same weekday next week
  int minutes = 0;     // minutes since midnight
};

// C library tm_wday (Sun=0..Sat=6) -> schedule day (Mon=0..Sun=6).
int to_schedule_day(int tm_wday);

// True when now_mins is 0..SCHEDULE_WINDOW_MINS after hour:minute and that
// slot time was not already fired. Writes the slot's minutes-since-midnight.
bool is_action_due(int hour, int minute, int now_mins, int last_fired_mins, int &out_sched_mins);

// Next enabled slot at or after now (same-minute slots count as upcoming).
// `skip` enabled slots are passed over first (1 = "skip next clean").
NextSlot next_slot(const Schedule &schedule, int day, int now_mins, int skip = 0);

// Duplicate-trigger guard for the current day.
class FiredGuard {
 public:
  void reset_for_day(int day);
  int fired(int slot) const { return this->fired_[slot]; }
  void claim(int slot, int sched_mins) { this->fired_[slot] = sched_mins; }
  void release(int slot) { this->fired_[slot] = -1; }

 protected:
  int day_{-1};
  int fired_[SCHEDULE_SLOTS_PER_DAY]{-1, -1};
};

}  // namespace neatocore
