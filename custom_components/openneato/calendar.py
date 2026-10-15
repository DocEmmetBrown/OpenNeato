"""Schedule calendar for the OpenNeato ESPHome firmware.

The bridge runs a weekly schedule of its own: two slots per weekday, each
with an on/off switch and a time (`Schedule Monday 1` / `Schedule Monday 1
time` ... on the ESPHome device, see docs/esphome-contract.md). This entity
shows those slots as weekly recurring events in the Calendar panel and turns
calendar edits back into slot changes, so the robot can be programmed from a
calendar while the schedule itself keeps running on the bridge.

What a calendar can do that the schedule cannot -- one-off dates, any number
of cleans a day -- is left to an ordinary calendar and the
`openneato/clean_on_calendar.yaml` blueprint.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
import datetime as dt
from typing import Any

from homeassistant.components.calendar import (
    CalendarEntity,
    CalendarEntityFeature,
    CalendarEvent,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import STATE_OFF, STATE_ON
from homeassistant.core import Event, EventStateChangedData, HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.event import async_track_state_change_event
from homeassistant.util import dt as dt_util

from .const import DOMAIN
from .entity import OpenNeatoEntity
from .esphome_backend import OpenNeatoEsphomeClient

DAYS = ("monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday")
RRULE_DAYS = ("MO", "TU", "WE", "TH", "FR", "SA", "SU")
SLOTS = (1, 2)
THIS_AND_FUTURE = "THISANDFUTURE"

DEFAULT_DURATION = dt.timedelta(hours=1)
MIN_DURATION = dt.timedelta(minutes=15)
MAX_DURATION = dt.timedelta(hours=4)
# How far ahead the `event` state looks for the next clean.
LOOKAHEAD = dt.timedelta(days=8)
# The ESPHome device confirms writes asynchronously; wait this long for the
# new state so the calendar redraws with it.
WRITE_TIMEOUT = 3.0


@dataclass(frozen=True)
class Slot:
    """One schedule slot on the bridge."""

    weekday: int  # 0 = Monday
    number: int
    enabled: bool
    time: dt.time | None

    @property
    def key(self) -> str:
        return f"schedule_{DAYS[self.weekday]}_{self.number}"

    @property
    def uid(self) -> str:
        return f"openneato_{DAYS[self.weekday]}_{self.number}"


def parse_uid(uid: str) -> tuple[int, int]:
    """(weekday, slot number) of an event uid."""
    try:
        _prefix, day, number = uid.split("_")
        return DAYS.index(day), int(number)
    except ValueError as err:
        raise HomeAssistantError(f"Unknown schedule event {uid!r}") from err


def parse_rrule(rrule: str | None, start: dt.datetime) -> list[int]:
    """Weekdays a weekly/daily rule runs on; anything else cannot be scheduled."""
    if not rrule:
        raise HomeAssistantError(
            "The robot's schedule repeats every week: set the event to repeat weekly (or daily). "
            "For one-off cleans, use another calendar with the OpenNeato cleaning-calendar blueprint."
        )
    parts = dict(part.split("=", 1) for part in rrule.removeprefix("RRULE:").split(";") if "=" in part)
    freq = parts.pop("FREQ", "")
    if parts.pop("INTERVAL", "1") != "1" or "COUNT" in parts or "UNTIL" in parts:
        raise HomeAssistantError("The robot's schedule cannot skip weeks or end on a date.")
    if freq == "DAILY" and not parts:
        return list(range(7))
    if freq == "WEEKLY" and set(parts) <= {"BYDAY", "WKST"}:
        if "BYDAY" not in parts:
            return [start.weekday()]
        try:
            return sorted({RRULE_DAYS.index(day.strip()) for day in parts["BYDAY"].split(",")})
        except ValueError as err:
            raise HomeAssistantError(f"Unsupported repeat rule {rrule!r}") from err
    raise HomeAssistantError("The robot's schedule only repeats weekly or daily.")


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up the schedule calendar (ESPHome firmware only)."""
    data = hass.data[DOMAIN][entry.entry_id]
    client = data["api"]
    if not isinstance(client, OpenNeatoEsphomeClient):
        return
    async_add_entities(
        [
            OpenNeatoScheduleCalendar(
                coordinator=data["coordinator"],
                client=client,
                serial=data["serial"],
                model=data["model"],
                sw_version=data["sw_version"],
                fw_version=data["fw_version"],
                via_device_id=data.get("via_device_id"),
            )
        ]
    )


class OpenNeatoScheduleCalendar(OpenNeatoEntity, CalendarEntity):
    """The bridge's weekly cleaning schedule as a calendar."""

    _attr_name = "Schedule"
    _attr_translation_key = "schedule"
    _attr_icon = "mdi:calendar-clock"
    _attr_supported_features = (
        CalendarEntityFeature.CREATE_EVENT
        | CalendarEntityFeature.DELETE_EVENT
        | CalendarEntityFeature.UPDATE_EVENT
    )

    def __init__(self, coordinator, client: OpenNeatoEsphomeClient, serial: str, **kwargs: Any) -> None:
        """Initialize the calendar."""
        super().__init__(coordinator, serial, **kwargs)
        self._client = client
        self._attr_unique_id = f"{serial}_schedule_calendar"

    async def async_added_to_hass(self) -> None:
        """Redraw as soon as a slot changes on the device."""
        await super().async_added_to_hass()
        watched = [
            entity_id
            for key in self._watched_keys()
            if (entity_id := self._client.entity_id(*key)) is not None
        ]
        if watched:
            self.async_on_remove(
                async_track_state_change_event(self.hass, watched, self._async_slot_changed)
            )

    @callback
    def _async_slot_changed(self, _event: Event[EventStateChangedData]) -> None:
        self.async_write_ha_state()

    @staticmethod
    def _watched_keys() -> list[tuple[str, str]]:
        keys = [("switch", "schedule"), ("switch", "skip_next_clean"), ("select", "navigation_mode")]
        for day in DAYS:
            for number in SLOTS:
                keys += [("switch", f"schedule_{day}_{number}"), ("time", f"schedule_{day}_{number}_time")]
        return keys

    # ── Reading the schedule ─────────────────────────────────────────

    def _slots(self) -> list[Slot]:
        slots = []
        for weekday, day in enumerate(DAYS):
            for number in SLOTS:
                raw = self._client.entity_state("time", f"schedule_{day}_{number}_time")
                try:
                    at = dt.time.fromisoformat(raw) if raw else None
                except ValueError:
                    at = None
                enabled = self._client.entity_state("switch", f"schedule_{day}_{number}") == STATE_ON
                slots.append(Slot(weekday, number, enabled, at))
        return slots

    def _duration(self) -> dt.timedelta:
        """How long a clean is drawn: the last clean's length, else an hour."""
        raw = self._client.entity_state("sensor", "clean_duration")
        try:
            seconds = float(raw) if raw else 0.0
        except ValueError:
            seconds = 0.0
        if seconds <= 0:
            return DEFAULT_DURATION
        quarter = 15 * 60
        rounded = dt.timedelta(seconds=-(-seconds // quarter) * quarter)
        return min(MAX_DURATION, max(MIN_DURATION, rounded))

    def _summary(self) -> tuple[str, str]:
        mode = self._client.entity_state("select", "navigation_mode")
        title = "Cleaning" if not mode or mode == "Normal" else f"Cleaning ({mode.lower()})"
        description = f"House clean, navigation mode {mode or 'Normal'}. Runs on the robot's bridge."
        return title, description

    def _occurrences(self, start: dt.datetime, end: dt.datetime) -> list[CalendarEvent]:
        """Scheduled cleans overlapping [start, end), oldest first."""
        if self._client.entity_state("switch", "schedule") != STATE_ON:
            return []
        tz = dt_util.get_default_time_zone()
        duration = self._duration()
        title, description = self._summary()
        slots = [s for s in self._slots() if s.enabled and s.time is not None]
        skip = self._client.entity_state("switch", "skip_next_clean") == STATE_ON
        skipped = self._next_start(slots, dt_util.now(), duration) if skip else None

        events: list[CalendarEvent] = []
        day = (start.astimezone(tz) - duration).date()
        last = end.astimezone(tz).date()
        while day <= last:
            for slot in slots:
                if slot.weekday != day.weekday():
                    continue
                begin = dt.datetime.combine(day, slot.time, tzinfo=tz)
                finish = begin + duration
                if finish <= start or begin >= end or (skipped is not None and begin == skipped):
                    continue
                events.append(
                    CalendarEvent(
                        start=begin,
                        end=finish,
                        summary=title,
                        description=description,
                        uid=slot.uid,
                        recurrence_id=begin.strftime("%Y%m%dT%H%M%S"),
                        rrule=f"FREQ=WEEKLY;BYDAY={RRULE_DAYS[slot.weekday]}",
                    )
                )
            day += dt.timedelta(days=1)
        events.sort(key=lambda e: e.start)
        return events

    @staticmethod
    def _next_start(slots: list[Slot], now: dt.datetime, duration: dt.timedelta) -> dt.datetime | None:
        """Start of the next scheduled clean that has not begun yet."""
        tz = dt_util.get_default_time_zone()
        today = now.astimezone(tz).date()
        best: dt.datetime | None = None
        for offset in range(8):
            day = today + dt.timedelta(days=offset)
            for slot in slots:
                if slot.weekday == day.weekday() and slot.time is not None:
                    begin = dt.datetime.combine(day, slot.time, tzinfo=tz)
                    if begin > now and (best is None or begin < best):
                        best = begin
            if best is not None:
                return best
        return best

    @property
    def event(self) -> CalendarEvent | None:
        """The clean in progress, or the next one."""
        now = dt_util.now()
        events = self._occurrences(now, now + LOOKAHEAD)
        return events[0] if events else None

    async def async_get_events(
        self, hass: HomeAssistant, start_date: dt.datetime, end_date: dt.datetime
    ) -> list[CalendarEvent]:
        """Scheduled cleans in a time range."""
        return self._occurrences(start_date, end_date)

    # ── Editing ──────────────────────────────────────────────────────

    async def async_create_event(self, **kwargs: Any) -> None:
        """Add a weekly clean on each day of the event's repeat rule."""
        start = self._start_of(kwargs)
        days = parse_rrule(kwargs.get("rrule"), start)
        writes = self._plan(days, start.time(), freed=None)
        await self._async_apply(writes)

    async def async_delete_event(
        self, uid: str, recurrence_id: str | None = None, recurrence_range: str | None = None
    ) -> None:
        """Remove a slot, or skip just its next occurrence."""
        weekday, number = parse_uid(uid)
        key = f"schedule_{DAYS[weekday]}_{number}"
        if recurrence_id and recurrence_range != THIS_AND_FUTURE and not self._is_next(uid, recurrence_id):
            raise HomeAssistantError(
                "The robot can only skip its next clean: delete the whole series, or skip the next one."
            )
        if recurrence_id and recurrence_range != THIS_AND_FUTURE:
            if self._client.entity_state("switch", "skip_next_clean") == STATE_ON:
                raise HomeAssistantError("The robot is already skipping its next clean.")
            await self._async_apply({("switch", "skip_next_clean"): STATE_ON})
            return
        await self._async_apply({("switch", key): STATE_OFF})

    async def async_update_event(
        self,
        uid: str,
        event: dict[str, Any],
        recurrence_id: str | None = None,
        recurrence_range: str | None = None,
    ) -> None:
        """Move a weekly clean to another time and/or other days."""
        if recurrence_id and recurrence_range != THIS_AND_FUTURE:
            raise HomeAssistantError(
                "Changes apply to every week: edit this and the following events instead."
            )
        weekday, number = parse_uid(uid)
        start = self._start_of(event)
        days = parse_rrule(event.get("rrule"), start)
        if days == [weekday]:
            await self._async_apply(
                {("time", f"schedule_{DAYS[weekday]}_{number}_time"): start.time().strftime("%H:%M:%S")}
            )
            return
        writes = self._plan(days, start.time(), freed=(weekday, number))
        writes.setdefault(("switch", f"schedule_{DAYS[weekday]}_{number}"), STATE_OFF)
        await self._async_apply(writes)

    @staticmethod
    def _start_of(event: dict[str, Any]) -> dt.datetime:
        start = event.get("dtstart")
        if not isinstance(start, dt.datetime):
            raise HomeAssistantError("Scheduled cleans need a start time, not a whole day.")
        return dt_util.as_local(start)

    def _is_next(self, uid: str, recurrence_id: str) -> bool:
        now = dt_util.now()
        upcoming = [e for e in self._occurrences(now, now + LOOKAHEAD) if e.start > now]
        return bool(upcoming) and upcoming[0].uid == uid and upcoming[0].recurrence_id == recurrence_id

    def _plan(
        self, days: list[int], at: dt.time, freed: tuple[int, int] | None
    ) -> dict[tuple[str, str], str]:
        """Slot writes for a clean at `at` on `days`, using free slots."""
        slots = self._slots()
        writes: dict[tuple[str, str], str] = {}
        for weekday in days:
            free = [
                s for s in slots
                if s.weekday == weekday and (not s.enabled or (s.weekday, s.number) == freed)
            ]
            if not free:
                raise HomeAssistantError(
                    f"Both cleans on {DAYS[weekday].capitalize()} are already scheduled; "
                    "move or delete one first."
                )
            slot = free[0]
            writes[("time", f"{slot.key}_time")] = at.strftime("%H:%M:%S")
            writes[("switch", slot.key)] = STATE_ON
        if self._client.entity_state("switch", "schedule") != STATE_ON:
            writes[("switch", "schedule")] = STATE_ON
        return writes

    async def _async_apply(self, writes: dict[tuple[str, str], str]) -> None:
        """Write slot changes to the device and wait for it to confirm them."""
        expected: dict[str, str] = {}
        for (entity_type, object_id), value in writes.items():
            entity_id = self._client.entity_id(entity_type, object_id)
            if entity_id is None:
                raise HomeAssistantError(f"The ESPHome device has no {object_id} entity")
            if entity_type == "time":
                await self.hass.services.async_call(
                    "time", "set_value", {"entity_id": entity_id, "time": value}, blocking=True
                )
            else:
                service = "turn_on" if value == STATE_ON else "turn_off"
                await self.hass.services.async_call("switch", service, {"entity_id": entity_id}, blocking=True)
            expected[entity_id] = value

        loop = asyncio.get_running_loop()
        deadline = loop.time() + WRITE_TIMEOUT
        while loop.time() < deadline:
            if all((s := self.hass.states.get(e)) is not None and s.state == v for e, v in expected.items()):
                break
            await asyncio.sleep(0.1)
        self.async_write_ha_state()
