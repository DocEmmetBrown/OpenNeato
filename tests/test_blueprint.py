"""Tests for the notification blueprint."""

from __future__ import annotations

from pathlib import Path
import shutil

from pytest_homeassistant_custom_component.common import MockConfigEntry, async_mock_service

from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.setup import async_setup_component

BLUEPRINT = Path(__file__).parent.parent / "blueprints" / "automation" / "openneato" / "notify.yaml"
EVENT = "event.neato_robot_event"


async def _setup(hass: HomeAssistant, tmp_path: Path, **inputs) -> list:
    hass.config.config_dir = str(tmp_path)
    dest = tmp_path / "blueprints" / "automation" / "openneato"
    dest.mkdir(parents=True)
    shutil.copy(BLUEPRINT, dest / "notify.yaml")

    entry = MockConfigEntry(domain="esphome")
    entry.add_to_hass(hass)
    device = dr.async_get(hass).async_get_or_create(
        config_entry_id=entry.entry_id, connections={(dr.CONNECTION_NETWORK_MAC, "aa:bb:cc:dd:ee:ff")}, name="Neato"
    )
    registry = er.async_get(hass)
    for domain, object_id in (("event", "neato_robot_event"), ("sensor", "neato_error_message"),
                              ("sensor", "neato_clean_duration"), ("sensor", "neato_clean_area")):
        registry.async_get_or_create(
            domain, "esphome", object_id, config_entry=entry, device_id=device.id, suggested_object_id=object_id
        )
    hass.states.async_set(EVENT, "unknown")
    hass.states.async_set("sensor.neato_error_message", "Please empty the dust bin")
    hass.states.async_set("sensor.neato_clean_duration", "3720")
    hass.states.async_set("sensor.neato_clean_area", "42.37")

    calls = async_mock_service(hass, "notify", "phone")
    assert await async_setup_component(
        hass,
        "automation",
        {
            "automation": {
                "use_blueprint": {
                    "path": "openneato/notify.yaml",
                    "input": {"robot_event": EVENT, "notify_action": "notify.phone", **inputs},
                }
            }
        },
    )
    await hass.async_block_till_done()
    return calls


def _fire(hass: HomeAssistant, when: str, event_type: str, **attrs) -> None:
    hass.states.async_set(EVENT, when, {"event_type": event_type, **attrs})


async def test_notifies_from_esphome_entities(hass: HomeAssistant, tmp_path: Path) -> None:
    """Without a message attribute the text comes from the device's sensors."""
    calls = await _setup(hass, tmp_path)

    _fire(hass, "2026-10-15T10:00:00+00:00", "alert")
    await hass.async_block_till_done()
    _fire(hass, "2026-10-15T10:05:00+00:00", "cleaning_done")
    await hass.async_block_till_done()

    assert [c.data for c in calls] == [
        {"title": "Neato", "message": "Please empty the dust bin"},
        {"title": "Neato", "message": "Cleaning done — 62min | 42.4m²"},
    ]


async def test_message_attribute_and_filters(hass: HomeAssistant, tmp_path: Path) -> None:
    """The HTTP entity's message wins; disabled types and reconnects are skipped."""
    calls = await _setup(hass, tmp_path, on_started=False)

    _fire(hass, "2026-10-15T10:00:00+00:00", "cleaning_started", message="Cleaning started")
    await hass.async_block_till_done()
    _fire(hass, "2026-10-15T10:01:00+00:00", "returned_to_base", message="Returned to base")
    await hass.async_block_till_done()
    hass.states.async_set(EVENT, "unavailable")
    await hass.async_block_till_done()
    _fire(hass, "2026-10-15T10:01:00+00:00", "error", message="Brush stuck")
    await hass.async_block_till_done()
    _fire(hass, "2026-10-15T10:02:00+00:00", "error", message="Brush stuck")
    await hass.async_block_till_done()

    assert [c.data["message"] for c in calls] == ["Brush stuck"]
