"""Tests for the ESPHome backend adapter."""

from __future__ import annotations

import pytest

from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant

from custom_components.openneato.api import OpenNeatoApiError, OpenNeatoConnectionError
from custom_components.openneato.esphome_backend import (
    OpenNeatoEsphomeClient,
    _parse_unique_id,
    is_openneato_esphome_device,
    openneato_esphome_devices,
)

from .conftest import FakeEsphomeDevice


@pytest.mark.parametrize(
    ("unique_id", "expected"),
    [
        ("aabbccddeeff-text_sensor-ui_state", ("text_sensor", "ui_state")),
        ("aabbccddeeff-sensor-Battery voltage", ("sensor", "battery_voltage")),
        ("aabbccddeeff/0/switch/Eco mode", ("switch", "eco_mode")),
        ("aabbccddeeff/3/switch/Eco mode", None),
        ("aabbccddeeff-sensor-battery@3", None),
        ("garbage", None),
    ],
)
def test_parse_unique_id(unique_id: str, expected) -> None:
    """All ESPHome unique-id schemes resolve to the contract object_id."""
    assert _parse_unique_id(unique_id) == expected


async def test_device_detection(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """Only ESPHome devices with the OpenNeato project are matched."""
    assert is_openneato_esphome_device(hass, esphome_device.device)
    assert openneato_esphome_devices(hass) == [esphome_device.device]


async def test_read_sections(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """Read sections are rebuilt from entity states in the HTTP API shape."""
    client = OpenNeatoEsphomeClient(hass, esphome_device.device.id)
    assert client.available()
    assert await client.get_state() == {"uiState": "UIMGR_STATE_IDLE", "robotState": "ST_C_Idle"}

    charger = await client.get_charger()
    assert charger["fuelPercent"] == 87
    assert charger["chargingActive"] is True
    assert charger["vBattV"] == pytest.approx(16.2)

    assert (await client.get_error())["hasError"] is False
    esphome_device.set("problem", "on")
    esphome_device.set("error_kind", "warning")
    esphome_device.set("error_code", "222")
    esphome_device.set("error_message", "Please empty the dust bin")
    error = await client.get_error()
    assert error == {
        "hasError": True,
        "kind": "warning",
        "errorCode": 222,
        "errorMessage": "Please empty the dust bin",
        "displayMessage": "Please empty the dust bin",
    }

    settings = await client.get_user_settings()
    assert settings["ecoMode"] is True
    assert settings["intenseClean"] is False
    assert settings["filterChange"] == 30 * 86400

    version = await client.get_robot_version()
    assert version["serialNumber"] == "OPS12345-ABC"
    assert version["modelName"] == "BotVacD7Connected"


async def test_not_ready_without_state(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """Missing ui_state or a disconnected device is a connection error."""
    client = OpenNeatoEsphomeClient(hass, esphome_device.device.id)
    esphome_device.set("ui_state", "unavailable")
    with pytest.raises(OpenNeatoConnectionError):
        await client.get_state()
    esphome_device.set("ui_state", "UIMGR_STATE_IDLE")
    esphome_device.entry.mock_state(hass, ConfigEntryState.NOT_LOADED)
    assert not client.available()
    with pytest.raises(OpenNeatoConnectionError):
        await client.get_state()


async def test_unsupported_sections_are_404(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """HTTP-only sections raise 404 so the coordinator drops them."""
    client = OpenNeatoEsphomeClient(hass, esphome_device.device.id)
    for method in (client.get_system, client.get_motors, client.get_schedule_next, client.get_history):
        with pytest.raises(OpenNeatoApiError) as err:
            await method()
        assert err.value.status == 404


async def test_actions(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """Commands map to ESPHome actions with the contract variables."""
    client = OpenNeatoEsphomeClient(hass, esphome_device.device.id)
    esphome_device.handlers["send_command"] = lambda data: {"response": "Pos: 1,2,3"}
    esphome_device.handlers["get_lidar"] = lambda data: {
        "rotationSpeed": 5.1,
        "validPoints": 2,
        "dist": [100, 0, 250],
        "intensity": [10, 0, 20],
        "error": [0, 1, 0],
    }

    await client.clean("house")
    await client.play_sound(19)
    await client.set_user_setting("EcoMode", "ON")
    assert await client.send_serial_command("GetRobotPos Smooth") == "Pos: 1,2,3"
    lidar = await client.get_lidar()

    assert esphome_device.calls[:4] == [
        ("clean", {"action": "house"}),
        ("play_sound", {"sound_id": 19}),
        ("set_user_setting", {"key": "EcoMode", "value": "ON"}),
        ("send_command", {"command": "GetRobotPos Smooth"}),
    ]
    assert lidar["rotationSpeed"] == pytest.approx(5.1)
    assert lidar["points"][2] == {"angle": 2, "dist": 250, "intensity": 20, "error": 0}
    assert len(lidar["points"]) == 3


async def test_missing_action_is_404(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """A device without the action (old firmware) reports 404."""
    client = OpenNeatoEsphomeClient(hass, esphome_device.device.id)
    hass.services.async_remove("esphome", "neato_c3_get_lidar")
    with pytest.raises(OpenNeatoApiError) as err:
        await client.get_lidar()
    assert err.value.status == 404


async def test_power_presses_buttons(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """Button-backed commands press the native ESPHome button."""
    pressed: list[str] = []

    async def press(call) -> None:
        pressed.extend(call.data["entity_id"] if isinstance(call.data["entity_id"], list) else [call.data["entity_id"]])

    hass.services.async_register("button", "press", press)
    client = OpenNeatoEsphomeClient(hass, esphome_device.device.id)
    await client.clear_errors()
    assert pressed == [esphome_device.entity_ids["clear_errors"]]
    with pytest.raises(OpenNeatoApiError):
        await client.power("restart")  # no restart_robot entity on this fake
