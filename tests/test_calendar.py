"""Tests for the schedule calendar and the cleaning-calendar blueprint."""

from __future__ import annotations

import datetime as dt
from pathlib import Path
import shutil
from unittest.mock import AsyncMock, patch

from freezegun.api import FrozenDateTimeFactory
import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry, async_fire_time_changed

from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import HomeAssistantError
from homeassistant.setup import async_setup_component
from homeassistant.util import dt as dt_util

from custom_components.openneato.calendar import DAYS, OpenNeatoScheduleCalendar, parse_rrule
from custom_components.openneato.const import BACKEND_ESPHOME, CONF_BACKEND, CONF_DEVICE_ID, DOMAIN

from .conftest import MAC, FakeEsphomeDevice

CALENDAR = "calendar.mock_title_schedule"  # device named after the config entry title
BLUEPRINT = Path(__file__).parent.parent / "blueprints" / "automation" / "openneato" / "clean_on_calendar.yaml"
# A Monday
MONDAY_6AM = dt.datetime(2026, 10, 19, 6, 0)


def local(day: int, hour: int, minute: int = 0) -> dt.datetime:
    """Local time on day `day` (0 = Monday 2026-10-19)."""
    return dt.datetime.combine(
        MONDAY_6AM.date() + dt.timedelta(days=day), dt.time(hour, minute), tzinfo=dt_util.get_default_time_zone()
    )


@pytest.fixture(autouse=True)
def no_frontend():
    with patch("custom_components.openneato.async_register_card", AsyncMock()), patch(
        "custom_components.openneato._async_register_frontend", AsyncMock()
    ):
        yield


@pytest.fixture
async def calendar(
    hass: HomeAssistant, esphome_device: FakeEsphomeDevice, freezer: FrozenDateTimeFactory, tmp_path
) -> OpenNeatoScheduleCalendar:
    """An ESPHome entry with Monday 07:30 and Wednesday 09:00 scheduled."""
    freezer.move_to(MONDAY_6AM.replace(tzinfo=dt_util.get_default_time_zone()))
    dev = esphome_device
    dev.add_entity("switch", "switch", "schedule", "on")
    dev.add_entity("switch", "switch", "skip_next_clean", "off")
    dev.add_entity("select", "select", "navigation_mode", "Normal")
    dev.add_entity("sensor", "sensor", "clean_duration", "3000")
    for day in DAYS:
        for n in (1, 2):
            dev.add_entity("switch", "switch", f"schedule_{day}_{n}", "off")
            dev.add_entity("time", "time", f"schedule_{day}_{n}_time", "00:00:00")
    dev.set("schedule_monday_1", "on")
    dev.set("schedule_monday_1_time", "07:30:00")
    dev.set("schedule_wednesday_1", "on")
    dev.set("schedule_wednesday_1_time", "09:00:00")

    def ids(call: ServiceCall) -> list[str]:
        value = call.data["entity_id"]
        return [value] if isinstance(value, str) else list(value)

    async def switch(call: ServiceCall) -> None:
        for entity_id in ids(call):
            hass.states.async_set(entity_id, "on" if call.service == "turn_on" else "off")

    async def set_time(call: ServiceCall) -> None:
        for entity_id in ids(call):
            hass.states.async_set(entity_id, str(call.data["time"]))

    hass.services.async_register("switch", "turn_on", switch)
    hass.services.async_register("switch", "turn_off", switch)
    hass.services.async_register("time", "set_value", set_time)
    dev.handlers["get_pose_batch"] = lambda data: {
        "boot": 1, "seq": 0, "first_seq": 1, "more": False, "current": None, "previous": None, "entries": []
    }

    hass.config.config_dir = str(tmp_path)
    entry = MockConfigEntry(
        domain=DOMAIN, unique_id=MAC, data={CONF_BACKEND: BACKEND_ESPHOME, CONF_DEVICE_ID: dev.device.id}
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    entity = hass.data["entity_components"]["calendar"].get_entity(CALENDAR)
    assert entity is not None
    yield entity
    await hass.config_entries.async_unload(entry.entry_id)


async def _week(entity: OpenNeatoScheduleCalendar, hass: HomeAssistant) -> list[tuple[str, dt.datetime]]:
    events = await entity.async_get_events(hass, local(0, 0), local(7, 0))
    return [(e.uid, e.start) for e in events]


@pytest.mark.parametrize(
    ("rrule", "days"),
    [
        ("FREQ=WEEKLY", [1]),
        ("FREQ=WEEKLY;BYDAY=MO,FR", [0, 4]),
        ("FREQ=DAILY", list(range(7))),
        ("RRULE:FREQ=WEEKLY;WKST=MO;BYDAY=SU", [6]),
    ],
)
def test_parse_rrule(rrule: str, days: list[int]) -> None:
    assert parse_rrule(rrule, local(1, 9)) == days


@pytest.mark.parametrize("rrule", [None, "FREQ=MONTHLY", "FREQ=WEEKLY;INTERVAL=2", "FREQ=WEEKLY;COUNT=3"])
def test_parse_rrule_rejects(rrule: str | None) -> None:
    with pytest.raises(HomeAssistantError):
        parse_rrule(rrule, local(1, 9))


async def test_shows_schedule(hass: HomeAssistant, calendar: OpenNeatoScheduleCalendar) -> None:
    """Enabled slots are weekly events; the state points at the next one."""
    events = await calendar.async_get_events(hass, local(0, 0), local(7, 0))
    assert [(e.uid, e.start) for e in events] == [
        ("openneato_monday_1", local(0, 7, 30)),
        ("openneato_wednesday_1", local(2, 9)),
    ]
    assert events[0].end - events[0].start == dt.timedelta(hours=1)  # 3000 s, rounded up to a quarter
    assert events[0].rrule == "FREQ=WEEKLY;BYDAY=MO"
    assert events[0].summary == "Cleaning"

    state = hass.states.get(CALENDAR)
    assert state.state == "off"
    assert state.attributes["start_time"] == "2026-10-19 07:30:00"


async def test_master_switch_and_skip(
    hass: HomeAssistant, calendar: OpenNeatoScheduleCalendar, esphome_device: FakeEsphomeDevice
) -> None:
    esphome_device.set("skip_next_clean", "on")
    assert await _week(calendar, hass) == [("openneato_wednesday_1", local(2, 9))]
    esphome_device.set("schedule", "off")
    assert await _week(calendar, hass) == []


async def test_create_weekly(
    hass: HomeAssistant, calendar: OpenNeatoScheduleCalendar, esphome_device: FakeEsphomeDevice
) -> None:
    """A weekly event fills a free slot on each of its days."""
    await calendar.async_create_event(
        dtstart=local(1, 18, 15), dtend=local(1, 19), summary="Clean", rrule="FREQ=WEEKLY;BYDAY=TU,MO"
    )
    states = hass.states
    assert states.get(esphome_device.entity_ids["schedule_tuesday_1"]).state == "on"
    assert states.get(esphome_device.entity_ids["schedule_tuesday_1_time"]).state == "18:15:00"
    # Monday's first slot is taken, so the second one is used.
    assert states.get(esphome_device.entity_ids["schedule_monday_2"]).state == "on"
    assert states.get(esphome_device.entity_ids["schedule_monday_2_time"]).state == "18:15:00"
    assert states.get(esphome_device.entity_ids["schedule_monday_1_time"]).state == "07:30:00"

    # Both Monday slots are now used.
    with pytest.raises(HomeAssistantError, match="Monday"):
        await calendar.async_create_event(
            dtstart=local(0, 12), dtend=local(0, 13), summary="Clean", rrule="FREQ=WEEKLY"
        )


async def test_create_turns_schedule_on(
    hass: HomeAssistant, calendar: OpenNeatoScheduleCalendar, esphome_device: FakeEsphomeDevice
) -> None:
    esphome_device.set("schedule", "off")
    await calendar.async_create_event(dtstart=local(4, 10), dtend=local(4, 11), summary="x", rrule="FREQ=WEEKLY")
    assert hass.states.get(esphome_device.entity_ids["schedule"]).state == "on"
    assert ("openneato_friday_1", local(4, 10)) in await _week(calendar, hass)


async def test_create_rejects_one_off_and_all_day(hass: HomeAssistant, calendar: OpenNeatoScheduleCalendar) -> None:
    with pytest.raises(HomeAssistantError, match="repeat"):
        await calendar.async_create_event(dtstart=local(1, 10), dtend=local(1, 11), summary="x")
    with pytest.raises(HomeAssistantError, match="whole day"):
        await calendar.async_create_event(
            dtstart=local(1, 0).date(), dtend=local(2, 0).date(), summary="x", rrule="FREQ=WEEKLY"
        )


async def test_delete(
    hass: HomeAssistant, calendar: OpenNeatoScheduleCalendar, esphome_device: FakeEsphomeDevice
) -> None:
    """Deleting the next occurrence skips it; deleting the series disables the slot."""
    with pytest.raises(HomeAssistantError, match="next"):
        await calendar.async_delete_event("openneato_wednesday_1", recurrence_id="20261021T090000")

    await calendar.async_delete_event("openneato_monday_1", recurrence_id="20261019T073000")
    assert hass.states.get(esphome_device.entity_ids["skip_next_clean"]).state == "on"
    assert hass.states.get(esphome_device.entity_ids["schedule_monday_1"]).state == "on"

    await calendar.async_delete_event("openneato_wednesday_1")
    assert hass.states.get(esphome_device.entity_ids["schedule_wednesday_1"]).state == "off"


async def test_update(
    hass: HomeAssistant, calendar: OpenNeatoScheduleCalendar, esphome_device: FakeEsphomeDevice
) -> None:
    """Changing the time keeps the slot; changing the day moves it."""
    await calendar.async_update_event(
        "openneato_monday_1",
        {"dtstart": local(0, 8, 45), "dtend": local(0, 9, 45), "summary": "x", "rrule": "FREQ=WEEKLY;BYDAY=MO"},
        recurrence_id="20261019T073000",
        recurrence_range="THISANDFUTURE",
    )
    assert hass.states.get(esphome_device.entity_ids["schedule_monday_1_time"]).state == "08:45:00"

    await calendar.async_update_event(
        "openneato_monday_1",
        {"dtstart": local(5, 10), "dtend": local(5, 11), "summary": "x", "rrule": "FREQ=WEEKLY;BYDAY=SA"},
    )
    assert hass.states.get(esphome_device.entity_ids["schedule_monday_1"]).state == "off"
    assert hass.states.get(esphome_device.entity_ids["schedule_saturday_1"]).state == "on"
    assert hass.states.get(esphome_device.entity_ids["schedule_saturday_1_time"]).state == "10:00:00"

    with pytest.raises(HomeAssistantError, match="every week"):
        await calendar.async_update_event(
            "openneato_saturday_1",
            {"dtstart": local(5, 11), "dtend": local(5, 12), "summary": "x", "rrule": "FREQ=WEEKLY;BYDAY=SA"},
            recurrence_id="20261024T100000",
        )


async def test_blueprint_starts_clean(
    hass: HomeAssistant,
    calendar: OpenNeatoScheduleCalendar,
    esphome_device: FakeEsphomeDevice,
    freezer: FrozenDateTimeFactory,
    tmp_path: Path,
) -> None:
    """The cleaning-calendar blueprint starts the robot when an event begins."""
    dest = tmp_path / "blueprints" / "automation" / "openneato"
    dest.mkdir(parents=True)
    shutil.copy(BLUEPRINT, dest / "clean_on_calendar.yaml")
    esphome_device.set("schedule_monday_1_time", "07:30:00")
    freezer.move_to(local(0, 7, 20))
    vacuum = next(
        e.entity_id
        for e in hass.data["entity_components"]["vacuum"].entities
        if e.entity_id.startswith("vacuum.")
    )
    assert hass.states.get(vacuum).state == "docked"

    assert await async_setup_component(
        hass,
        "automation",
        {
            "automation": {
                "use_blueprint": {
                    "path": "openneato/clean_on_calendar.yaml",
                    "input": {"calendar": CALENDAR, "vacuum": vacuum},
                }
            }
        },
    )
    await hass.async_block_till_done()

    freezer.move_to(local(0, 7, 30) + dt.timedelta(seconds=1))
    async_fire_time_changed(hass)
    await hass.async_block_till_done()
    assert ("clean", {"action": "house"}) in esphome_device.calls

    await hass.services.async_call("automation", "turn_off", {"entity_id": "all"}, blocking=True)
