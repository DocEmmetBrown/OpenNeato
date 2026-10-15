"""Shared fixtures for the OpenNeato integration tests."""

from __future__ import annotations

from collections.abc import Callable, Generator
from dataclasses import dataclass, field
from types import SimpleNamespace
from typing import Any

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er

MAC = "aa:bb:cc:dd:ee:ff"
NODE = "neato-c3"

# (esphome entity type, HA domain, object_id) -> initial state
DEFAULT_STATES: dict[tuple[str, str, str], str] = {
    ("text_sensor", "sensor", "ui_state"): "UIMGR_STATE_IDLE",
    ("text_sensor", "sensor", "robot_state"): "ST_C_Idle",
    ("text_sensor", "sensor", "error_message"): "",
    ("text_sensor", "sensor", "error_kind"): "",
    ("text_sensor", "sensor", "model"): "BotVacD7Connected",
    ("text_sensor", "sensor", "serial_number"): "OPS12345-ABC",
    ("text_sensor", "sensor", "robot_firmware"): "4.5.3",
    ("sensor", "sensor", "battery"): "87",
    ("sensor", "sensor", "battery_voltage"): "16.2",
    ("sensor", "sensor", "error_code"): "200",
    ("binary_sensor", "binary_sensor", "charging"): "on",
    ("binary_sensor", "binary_sensor", "external_power"): "on",
    ("binary_sensor", "binary_sensor", "problem"): "off",
    ("switch", "switch", "eco_mode"): "on",
    ("switch", "switch", "intense_clean"): "off",
    ("number", "number", "filter_change_interval"): "30",
    ("button", "button", "clear_errors"): "unknown",
    ("event", "event", "robot_event"): "unknown",
}


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations, hass: HomeAssistant):
    """Load custom_components/ in every test.

    The manifest depends on frontend/http/websocket_api, which need the
    frontend package; mark them loaded instead, the card is patched out.
    """
    hass.config.components.update({"frontend", "http", "websocket_api", "lovelace"})


@dataclass
class FakeEsphomeDevice:
    """An ESPHome device running the OpenNeato firmware, as HA sees it."""

    hass: HomeAssistant
    entry: MockConfigEntry
    device: dr.DeviceEntry
    entity_ids: dict[str, str] = field(default_factory=dict)
    calls: list[tuple[str, dict[str, Any]]] = field(default_factory=list)
    handlers: dict[str, Callable[[dict[str, Any]], dict[str, Any]]] = field(default_factory=dict)

    def set(self, object_id: str, value: str) -> None:
        """Set the state of one contract entity."""
        self.hass.states.async_set(self.entity_ids[object_id], value)

    def register_action(self, action: str) -> None:
        async def handle(call: ServiceCall) -> dict[str, Any] | None:
            self.calls.append((action, dict(call.data)))
            handler = self.handlers.get(action)
            result = handler(dict(call.data)) if handler else {}
            return result if call.return_response else None

        self.hass.services.async_register(
            "esphome",
            f"{NODE.replace('-', '_')}_{action}",
            handle,
            supports_response=SupportsResponse.OPTIONAL,
        )


@pytest.fixture
def esphome_device(hass: HomeAssistant) -> Generator[FakeEsphomeDevice]:
    """A loaded ESPHome config entry with an OpenNeato device and its entities."""
    entry = MockConfigEntry(domain="esphome", data={"device_name": NODE}, unique_id=MAC)
    entry.add_to_hass(hass)
    entry.mock_state(hass, ConfigEntryState.LOADED)
    entry.runtime_data = SimpleNamespace(available=True, device_info=SimpleNamespace(name=NODE))

    device = dr.async_get(hass).async_get_or_create(
        config_entry_id=entry.entry_id,
        connections={(dr.CONNECTION_NETWORK_MAC, MAC)},
        manufacturer="openneato",
        model="esphome",
        name="Neato",
    )
    fake = FakeEsphomeDevice(hass, entry, device)
    registry = er.async_get(hass)
    for (etype, domain, object_id), value in DEFAULT_STATES.items():
        ent = registry.async_get_or_create(
            domain,
            "esphome",
            f"{MAC.replace(':', '')}-{etype}-{object_id}",
            config_entry=entry,
            device_id=device.id,
            suggested_object_id=f"neato_{object_id}",
        )
        fake.entity_ids[object_id] = ent.entity_id
        hass.states.async_set(ent.entity_id, value)

    for action in (
        "clean",
        "play_sound",
        "set_user_setting",
        "send_command",
        "get_lidar",
        "get_pose_batch",
        "manual_move",
    ):
        fake.register_action(action)
    yield fake
    # Not really loaded: keep HA from unloading the real esphome integration.
    entry.mock_state(hass, ConfigEntryState.NOT_LOADED)
