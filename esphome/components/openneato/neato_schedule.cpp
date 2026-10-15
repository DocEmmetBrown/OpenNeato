// Ported from OpenNeato firmware/src/scheduler.cpp (MIT)
#include "neato_schedule.h"

namespace neatocore {

const char *const SCHEDULE_DAY_KEYS[SCHEDULE_DAYS] = {"monday", "tuesday",  "wednesday", "thursday",
                                                      "friday", "saturday", "sunday"};
const char *const SCHEDULE_DAY_NAMES[SCHEDULE_DAYS] = {"Monday", "Tuesday",  "Wednesday", "Thursday",
                                                       "Friday", "Saturday", "Sunday"};

int to_schedule_day(int tm_wday) { return (tm_wday + 6) % 7; }

bool is_action_due(int hour, int minute, int now_mins, int last_fired_mins, int &out_sched_mins) {
  out_sched_mins = hour * 60 + minute;
  int elapsed = now_mins - out_sched_mins;
  if (elapsed < 0 || elapsed > SCHEDULE_WINDOW_MINS)
    return false;
  return out_sched_mins != last_fired_mins;
}

NextSlot next_slot(const Schedule &schedule, int day, int now_mins, int skip) {
  NextSlot result;
  if (!schedule.enabled)
    return result;
  // Two weeks covers skipping one enabled slot.
  for (int offset = 0; offset <= SCHEDULE_DAYS * 2; offset++) {
    int d = (day + offset) % SCHEDULE_DAYS;
    // This day's eligible slots, in time order (ties: lower slot index first).
    int order[SCHEDULE_SLOTS_PER_DAY];
    int count = 0;
    for (int si = 0; si < SCHEDULE_SLOTS_PER_DAY; si++) {
      const ScheduleSlot &slot = schedule.slots[d][si];
      int mins = slot.hour * 60 + slot.minute;
      if (!slot.on || (offset == 0 && mins < now_mins))
        continue;
      int pos = count++;
      while (pos > 0) {
        const ScheduleSlot &prev = schedule.slots[d][order[pos - 1]];
        if (prev.hour * 60 + prev.minute <= mins)
          break;
        order[pos] = order[pos - 1];
        pos--;
      }
      order[pos] = si;
    }
    for (int i = 0; i < count; i++) {
      if (skip > 0) {
        skip--;
        continue;
      }
      const ScheduleSlot &slot = schedule.slots[d][order[i]];
      result.found = true;
      result.day = d;
      result.slot = order[i];
      result.day_offset = offset;
      result.minutes = slot.hour * 60 + slot.minute;
      return result;
    }
  }
  return result;
}

void FiredGuard::reset_for_day(int day) {
  if (day == this->day_)
    return;
  this->day_ = day;
  for (int &f : this->fired_)
    f = -1;
}

}  // namespace neatocore
