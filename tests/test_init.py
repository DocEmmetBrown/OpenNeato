"""Tests for setting up OpenNeato entries."""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er

from custom_components.openneato.const import BACKEND_ESPHOME, CONF_BACKEND, CONF_DEVICE_ID, DOMAIN

from .conftest import MAC, FakeEsphomeDevice


@pytest.fixture(autouse=True)
def no_frontend(hass: HomeAssistant):
    """The card and its websocket API need the real frontend; skip them."""
    with patch("custom_components.openneato.async_register_card", AsyncMock()), patch(
        "custom_components.openneato._async_register_frontend", AsyncMock()
    ):
        yield


async def test_esphome_entry(hass: HomeAssistant, esphome_device: FakeEsphomeDevice, tmp_path) -> None:
    """An ESPHome entry adds only the vacuum, on the ESPHome device."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=MAC,
        data={CONF_BACKEND: BACKEND_ESPHOME, CONF_DEVICE_ID: esphome_device.device.id},
    )
    entry.add_to_hass(hass)
    esphome_device.handlers["get_pose_batch"] = lambda data: {
        "boot": 1, "seq": 0, "first_seq": 1, "more": False, "current": None, "previous": None, "entries": []
    }
    hass.config.config_dir = str(tmp_path)

    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.LOADED
    assert entry.data["serial"] == "OPS12345-ABC"

    entities = er.async_entries_for_config_entry(er.async_get(hass), entry.entry_id)
    assert [e.domain for e in entities] == ["vacuum"]
    vacuum = entities[0]
    assert vacuum.unique_id == "OPS12345-ABC_vacuum"
    robot = dr.async_get(hass).async_get(vacuum.device_id)
    assert robot.identifiers == {(DOMAIN, "OPS12345-ABC")}
    # Reached through the ESPHome bridge, whose identity stays untouched.
    assert robot.via_device_id == esphome_device.device.id
    assert dr.async_get(hass).async_get(esphome_device.device.id).manufacturer == "openneato"

    state = hass.states.get(vacuum.entity_id)
    assert state.state == "docked"
    assert state.attributes["fan_speed"] == "eco"

    await hass.services.async_call("vacuum", "start", {"entity_id": vacuum.entity_id}, blocking=True)
    assert ("clean", {"action": "house"}) in esphome_device.calls
    # The recorder caught up with the device at startup.
    assert any(action == "get_pose_batch" for action, _ in esphome_device.calls)

    assert await hass.config_entries.async_unload(entry.entry_id)
    assert entry.state is ConfigEntryState.NOT_LOADED


async def test_migrated_entry_drops_http_entities(
    hass: HomeAssistant, esphome_device: FakeEsphomeDevice, tmp_path
) -> None:
    """Entities of an entry migrated from the HTTP firmware are cleaned up."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=MAC,
        data={CONF_BACKEND: BACKEND_ESPHOME, CONF_DEVICE_ID: esphome_device.device.id, "serial": "OPS12345-ABC"},
    )
    entry.add_to_hass(hass)
    registry = er.async_get(hass)
    registry.async_get_or_create("vacuum", DOMAIN, "OPS12345-ABC_vacuum", config_entry=entry)
    registry.async_get_or_create("sensor", DOMAIN, "OPS12345-ABC_battery", config_entry=entry)
    hass.config.config_dir = str(tmp_path)

    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()

    entities = er.async_entries_for_config_entry(registry, entry.entry_id)
    assert [(e.domain, e.unique_id) for e in entities] == [("vacuum", "OPS12345-ABC_vacuum")]


async def test_esphome_entry_not_ready_when_disconnected(
    hass: HomeAssistant, esphome_device: FakeEsphomeDevice
) -> None:
    """The entry retries until the ESPHome device is connected."""
    esphome_device.entry.runtime_data.available = False
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=MAC,
        data={CONF_BACKEND: BACKEND_ESPHOME, CONF_DEVICE_ID: esphome_device.device.id},
    )
    entry.add_to_hass(hass)
    await hass.config_entries.async_setup(entry.entry_id)
    assert entry.state is ConfigEntryState.SETUP_RETRY


async def test_http_entry_has_event_entity(hass: HomeAssistant) -> None:
    """The HTTP backend keeps all its platforms and gains the robot event."""
    entry = MockConfigEntry(domain=DOMAIN, unique_id="SER1", data={"host": "10.0.0.2", "serial": "SER1"})
    entry.add_to_hass(hass)

    patches = {
        "get_firmware_version": {"version": "1.14.1", "repositoryUrl": "https://github.com/Leicas/OpenNeato"},
        "get_robot_version": {"serialNumber": "SER1", "modelName": "BotVacD7Connected"},
        "get_state": {"uiState": "UIMGR_STATE_IDLE", "robotState": "ST_C_Idle"},
        "get_charger": {"fuelPercent": 90, "chargingActive": True},
        "get_error": {"hasError": False, "errorCode": 200},
        "get_history": [],
    }
    with patch.multiple(
        "custom_components.openneato.api.OpenNeatoApiClient",
        **{name: AsyncMock(return_value=value) for name, value in patches.items()},
        **{
            name: AsyncMock(return_value={})
            for name in (
                "get_user_settings", "get_system", "get_settings", "get_motors", "get_sensors",
                "get_battery_analog", "get_battery_warranty", "get_schedule_next",
            )
        },
    ):
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    entities = er.async_entries_for_config_entry(er.async_get(hass), entry.entry_id)
    domains = {e.domain for e in entities}
    assert {"vacuum", "sensor", "event"} <= domains
    event = next(e for e in entities if e.domain == "event")
    assert event.unique_id == "SER1_robot_event"
