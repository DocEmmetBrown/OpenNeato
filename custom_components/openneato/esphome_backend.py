"""ESPHome backend for the OpenNeato integration.

`OpenNeatoEsphomeClient` exposes the subset of `OpenNeatoApiClient` that the
coordinator, vacuum entity, replay card and LIDAR mapper rely on, on top of a
robot running the OpenNeato ESPHome firmware (`esphome/components/openneato`).

* Read sections (`state`, `charger`, `error`, `user_settings`) are rebuilt
  from the ESPHome device's entity states, which the ESPHome integration
  already keeps current, so a coordinator tick costs nothing on the robot.
* Writes and on-demand reads go through the device's ESPHome actions
  (`esphome.<node>_<action>`), which answer with JSON.
* Cleaning history is recorded on the Home Assistant side by
  `EsphomeSessionRecorder` (see esphome_history.py) in the same JSONL format
  the HTTP firmware serves, so the replay card and mapper work unchanged.

Every method the HTTP client has but this backend cannot serve raises
`OpenNeatoApiError(status=404)`, which the coordinator already treats as
"this firmware does not have that endpoint".

The entity and action names used here are a contract with the firmware; see
docs/esphome-contract.md.
"""

from __future__ import annotations

import logging
import re
from typing import Any

from homeassistant.config_entries import ConfigEntryState
from homeassistant.const import STATE_ON, STATE_UNAVAILABLE, STATE_UNKNOWN
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er

from .api import OpenNeatoApiError, OpenNeatoConnectionError
from .esphome_history import EsphomeSessionRecorder

_LOGGER = logging.getLogger(__name__)

ESPHOME_DOMAIN = "esphome"
# `esphome: project: name: openneato.esphome` -> HA reports these two
ESPHOME_MANUFACTURER = "openneato"
ESPHOME_MODEL = "esphome"

# Robot user-setting keys (SetUserSettings) -> contract switch object_id
USER_SETTING_SWITCHES: dict[str, str] = {
    "ecoMode": "eco_mode",
    "intenseClean": "intense_clean",
    "binFullDetect": "bin_full_detection",
    "wallEnable": "wall_following",
    "buttonClick": "button_click_sound",
    "melodies": "melodies",
    "warnings": "warning_sounds",
    "stealthLed": "stealth_led",
}

SECONDS_PER_DAY = 86400

_NON_ID_CHARS = re.compile(r"[^a-z0-9_]")


def normalize_object_id(name: str) -> str:
    """Snake-case an entity name the way ESPHome derives object_ids."""
    return _NON_ID_CHARS.sub("_", name.strip().lower().replace(" ", "_"))


def _parse_unique_id(unique_id: str) -> tuple[str, str] | None:
    """(entity type, object_id) of a main-device ESPHome entity unique_id."""
    if "/" in unique_id:
        parts = unique_id.split("/", 3)
        if len(parts) != 4 or parts[1] != "0":
            return None
        return parts[2], normalize_object_id(parts[3])
    parts = unique_id.split("-", 2)
    if len(parts) != 3 or "@" in parts[2]:
        return None
    return parts[1], normalize_object_id(parts[2])


def device_config_entry_ids(device: dr.DeviceEntry) -> set[str]:
    """Config entries of a device, across device-registry versions.

    Home Assistant 2026.10 gives every device a single `config_entry_id`
    and deprecates `config_entries`; older versions only have the latter.
    """
    entry_id = getattr(device, "config_entry_id", None)
    if entry_id is not None:
        return {entry_id}
    return set(getattr(device, "config_entries", ()) or ())


def device_mac(device: dr.DeviceEntry) -> str | None:
    """The device's MAC address connection, formatted."""
    for kind, value in device.connections:
        if kind == dr.CONNECTION_NETWORK_MAC:
            return dr.format_mac(value)
    return None


def is_openneato_esphome_device(hass: HomeAssistant, device: dr.DeviceEntry) -> bool:
    """True for an ESPHome device running the OpenNeato ESPHome firmware."""
    if (device.manufacturer or "").lower() != ESPHOME_MANUFACTURER:
        return False
    if (device.model or "").lower() != ESPHOME_MODEL:
        return False
    return any(
        (entry := hass.config_entries.async_get_entry(entry_id)) is not None
        and entry.domain == ESPHOME_DOMAIN
        for entry_id in device_config_entry_ids(device)
    )


def openneato_esphome_devices(hass: HomeAssistant) -> list[dr.DeviceEntry]:
    """All ESPHome devices running the OpenNeato ESPHome firmware."""
    registry = dr.async_get(hass)
    return [
        device
        for entry in hass.config_entries.async_entries(ESPHOME_DOMAIN)
        for device in dr.async_entries_for_config_entry(registry, entry.entry_id)
        if is_openneato_esphome_device(hass, device)
    ]


def _not_supported(what: str) -> OpenNeatoApiError:
    return OpenNeatoApiError(f"{what} is not available on the ESPHome firmware", status=404)


class OpenNeatoEsphomeClient:
    """`OpenNeatoApiClient` look-alike backed by an ESPHome device."""

    # Coordinator sections covered by native ESPHome entities instead.
    unsupported_sections = frozenset(
        {"system", "settings", "motors", "sensors", "analog", "warranty", "schedule_next"}
    )

    def __init__(
        self,
        hass: HomeAssistant,
        device_id: str,
        recorder: EsphomeSessionRecorder | None = None,
    ) -> None:
        """Initialize the client for one ESPHome device."""
        self.hass = hass
        self.device_id = device_id
        self.recorder = recorder

    # ── Device / entity resolution ───────────────────────────────────

    @property
    def device(self) -> dr.DeviceEntry | None:
        """The ESPHome device entry."""
        return dr.async_get(self.hass).async_get(self.device_id)

    def _entities(self) -> dict[tuple[str, str], str]:
        """Map (esphome entity type, object_id) -> entity_id for the device.

        ESPHome unique IDs are `{mac}-{type}-{object_id}` (v1, current),
        `{mac}-{type}-{name}` (v2) or `{mac}/{device_id}/{type}/{name}` (v3);
        the last part is normalized so all resolve to the contract's
        object_id. Not cached: the registry lookup is cheap and the device's
        entities can change on every OTA.
        """
        mapping: dict[tuple[str, str], str] = {}
        for entity in er.async_entries_for_device(
            er.async_get(self.hass), self.device_id, include_disabled_entities=True
        ):
            if entity.platform != ESPHOME_DOMAIN:
                continue
            parsed = _parse_unique_id(entity.unique_id)
            if parsed is not None:
                mapping[parsed] = entity.entity_id
        return mapping

    def entity_id(self, entity_type: str, object_id: str) -> str | None:
        """Entity id of one contract entity, or None if the device lacks it."""
        return self._entities().get((entity_type, object_id))

    def _state(self, entity_type: str, object_id: str) -> str | None:
        """Raw state of a contract entity, None when unknown/unavailable."""
        entity_id = self.entity_id(entity_type, object_id)
        if entity_id is None:
            return None
        state = self.hass.states.get(entity_id)
        if state is None or state.state in (STATE_UNAVAILABLE, STATE_UNKNOWN):
            return None
        return state.state

    def text(self, object_id: str) -> str:
        """State of a contract text sensor, empty when unknown."""
        return self._state("text_sensor", object_id) or ""

    def _number(self, entity_type: str, object_id: str) -> float | None:
        raw = self._state(entity_type, object_id)
        if raw is None:
            return None
        try:
            return float(raw)
        except ValueError:
            return None

    def _bool(self, entity_type: str, object_id: str) -> bool:
        return self._state(entity_type, object_id) == STATE_ON

    def _esphome_entry(self):
        device = self.device
        if device is None:
            return None
        for entry_id in device_config_entry_ids(device):
            entry = self.hass.config_entries.async_get_entry(entry_id)
            if entry is not None and entry.domain == ESPHOME_DOMAIN:
                return entry
        return None

    def _node_name(self) -> str:
        entry = self._esphome_entry()
        if entry is None:
            raise OpenNeatoConnectionError("The ESPHome device is no longer set up")
        name: str | None = None
        runtime = getattr(entry, "runtime_data", None)
        device_info = getattr(runtime, "device_info", None) if runtime else None
        if device_info is not None:
            name = getattr(device_info, "name", None)
        name = name or entry.data.get("device_name")
        if not name:
            raise OpenNeatoConnectionError("The ESPHome device has not connected yet")
        return name.replace("-", "_")

    def available(self) -> bool:
        """True when the ESPHome device is connected."""
        entry = self._esphome_entry()
        if entry is None or entry.state is not ConfigEntryState.LOADED:
            return False
        runtime = getattr(entry, "runtime_data", None)
        return bool(getattr(runtime, "available", True))

    def _require_available(self) -> None:
        if not self.available():
            raise OpenNeatoConnectionError("The OpenNeato ESPHome device is not connected")
        if self._state("text_sensor", "ui_state") is None:
            raise OpenNeatoConnectionError("The robot has not reported its state yet")

    async def call_action(
        self, action: str, data: dict[str, Any] | None = None, *, response: bool = True
    ) -> dict[str, Any]:
        """Call one of the device's ESPHome actions and return its JSON reply."""
        if not self.available():
            raise OpenNeatoConnectionError("The OpenNeato ESPHome device is not connected")
        service = f"{self._node_name()}_{action}"
        if not self.hass.services.has_service(ESPHOME_DOMAIN, service):
            raise OpenNeatoApiError(
                f"The ESPHome device has no action {action!r}; is it running the OpenNeato "
                "ESPHome firmware, and does Home Assistant allow it to register actions?",
                status=404,
            )
        try:
            result = await self.hass.services.async_call(
                ESPHOME_DOMAIN,
                service,
                data or {},
                blocking=True,
                return_response=response,
            )
        except HomeAssistantError as err:
            raise OpenNeatoApiError(f"{action} failed: {err}") from err
        return dict(result or {})

    async def _press(self, object_id: str) -> dict[str, Any]:
        entity_id = self.entity_id("button", object_id)
        if entity_id is None:
            raise _not_supported(object_id)
        await self.hass.services.async_call(
            "button", "press", {"entity_id": entity_id}, blocking=True
        )
        return {"ok": True}

    # ── Lifecycle (no-ops kept for interface parity) ─────────────────

    @property
    def base_url(self) -> str:
        """No HTTP base URL; the card's map view never needs one."""
        return ""

    # ── Read sections ────────────────────────────────────────────────

    async def get_state(self) -> dict[str, Any]:
        """uiState/robotState, as /api/state."""
        self._require_available()
        return {"uiState": self.text("ui_state"), "robotState": self.text("robot_state")}

    async def get_charger(self) -> dict[str, Any]:
        """Battery / charger flags, as /api/charger."""
        self._require_available()
        fuel = self._number("sensor", "battery")
        volts = self._number("sensor", "battery_voltage")
        ext = self._number("sensor", "external_voltage")
        temp = self._number("sensor", "battery_temperature")
        return {
            "fuelPercent": int(fuel) if fuel is not None else -1,
            "chargingActive": self._bool("binary_sensor", "charging"),
            "extPwrPresent": self._bool("binary_sensor", "external_power"),
            "batteryOverTemp": self._bool("binary_sensor", "battery_overheating"),
            "batteryFailure": self._bool("binary_sensor", "battery_failure"),
            "vBattV": volts if volts is not None else 0.0,
            "vExtV": ext if ext is not None else 0.0,
            "battTempC": int(temp) if temp is not None else 0,
        }

    async def get_error(self) -> dict[str, Any]:
        """Current error/alert, as /api/error."""
        self._require_available()
        code = self._number("sensor", "error_code")
        message = self.text("error_message")
        kind = self.text("error_kind")
        has_error = self._bool("binary_sensor", "problem") or bool(kind)
        return {
            "hasError": has_error,
            "kind": kind or ("error" if has_error else ""),
            "errorCode": int(code) if code is not None else 200,
            "errorMessage": message,
            "displayMessage": message,
        }

    async def get_user_settings(self) -> dict[str, Any]:
        """Robot user settings, as /api/user-settings."""
        self._require_available()
        settings: dict[str, Any] = {
            key: self._bool("switch", object_id)
            for key, object_id in USER_SETTING_SWITCHES.items()
        }
        filter_days = self._number("number", "filter_change_interval")
        brush_days = self._number("number", "brush_change_interval")
        dirt_bin = self._number("number", "dirt_bin_reminder")
        if filter_days is not None:
            settings["filterChange"] = int(filter_days * SECONDS_PER_DAY)
        if brush_days is not None:
            settings["brushChange"] = int(brush_days * SECONDS_PER_DAY)
        if dirt_bin is not None:
            settings["dirtBin"] = int(dirt_bin)
        return settings

    async def get_robot_version(self) -> dict[str, Any]:
        """Robot identity, as /api/version."""
        return {
            "modelName": self.text("model"),
            "serialNumber": self.text("serial_number"),
            "softwareVersion": self.text("robot_firmware"),
            "ldsVersion": self.text("lds_firmware"),
            "mainBoardVersion": self.text("main_board"),
        }

    async def get_firmware_version(self) -> dict[str, Any]:
        """Bridge firmware, as /api/firmware/version."""
        device = self.device
        return {
            "version": (device.sw_version if device else None) or "esphome",
            "backend": "esphome",
        }

    # ── History (recorded on the Home Assistant side) ────────────────

    async def get_history(self) -> list[dict[str, Any]]:
        """Recorded sessions, as /api/history."""
        if self.recorder is None:
            raise _not_supported("History")
        return self.recorder.list_sessions()

    async def get_history_session(self, filename: str) -> str:
        """Raw JSONL of one session, as /api/history/<name>."""
        if self.recorder is None:
            raise _not_supported("History")
        return await self.recorder.async_read_session(filename)

    async def delete_history_session(self, filename: str) -> None:
        """Delete one recorded session."""
        if self.recorder is None:
            raise _not_supported("History")
        await self.recorder.async_delete_session(filename)

    # ── On-demand reads via actions ──────────────────────────────────

    async def get_lidar(self) -> dict[str, Any]:
        """One LIDAR scan, expanded to the /api/lidar shape."""
        data = await self.call_action("get_lidar")
        dist = list(data.get("dist") or [])
        intensity = list(data.get("intensity") or [])
        error = list(data.get("error") or [])
        points = [
            {
                "angle": angle,
                "dist": int(dist[angle]) if angle < len(dist) else 0,
                "intensity": int(intensity[angle]) if angle < len(intensity) else 0,
                "error": int(error[angle]) if angle < len(error) else 1,
            }
            for angle in range(len(dist))
        ]
        return {
            "rotationSpeed": float(data.get("rotationSpeed") or 0.0),
            "validPoints": int(data.get("validPoints") or 0),
            "points": points,
        }

    async def send_serial_command(self, cmd: str) -> str:
        """Run a raw serial command and return the robot's text reply."""
        data = await self.call_action("send_command", {"command": cmd})
        return str(data.get("response", ""))

    # ── Commands ─────────────────────────────────────────────────────

    async def clean(self, action: str) -> dict[str, Any]:
        """house/spot/pause/stop/dock, with the HTTP firmware's semantics."""
        await self.call_action("clean", {"action": action}, response=False)
        return {"ok": True}

    async def play_sound(self, sound_id: int) -> dict[str, Any]:
        """Play a robot sound (0-20)."""
        await self.call_action("play_sound", {"sound_id": int(sound_id)}, response=False)
        return {"ok": True}

    async def set_user_setting(self, key: str, value: str) -> dict[str, Any]:
        """SetUserSettings <key> <value>."""
        await self.call_action(
            "set_user_setting", {"key": key, "value": value}, response=False
        )
        return {"ok": True}

    async def clear_errors(self) -> dict[str, Any]:
        """Clear the robot's errors and alerts."""
        return await self._press("clear_errors")

    async def new_battery(self) -> dict[str, Any]:
        """Reset the fuel gauge after fitting a new battery."""
        return await self._press("new_battery")

    async def power(self, action: str) -> dict[str, Any]:
        """Robot power-cycle or shutdown."""
        if action == "restart":
            return await self._press("restart_robot")
        if action == "shutdown":
            return await self._press("shut_down_robot")
        raise _not_supported(f"Power action {action!r}")

    # ── HTTP-firmware-only endpoints ─────────────────────────────────

    async def get_schedule_next(self) -> dict[str, Any]:
        """The schedule lives in native ESPHome entities."""
        raise _not_supported("Schedule")

    async def get_battery_analog(self) -> dict[str, Any]:
        """Native ESPHome sensors cover this."""
        raise _not_supported("Analog battery data")

    async def get_battery_warranty(self) -> dict[str, Any]:
        """Native ESPHome sensors cover this."""
        raise _not_supported("Battery warranty data")

    async def get_motors(self) -> dict[str, Any]:
        """Native ESPHome sensors cover this."""
        raise _not_supported("Motor data")

    async def get_system(self) -> dict[str, Any]:
        """Native ESPHome diagnostics cover this."""
        raise _not_supported("Bridge system data")

    async def get_sensors(self) -> dict[str, Any]:
        """Native ESPHome binary sensors cover this."""
        raise _not_supported("Digital sensors")

    async def get_settings(self) -> dict[str, Any]:
        """Bridge settings are ESPHome YAML / native entities."""
        raise _not_supported("Bridge settings")

    async def update_settings(self, settings: dict[str, Any]) -> dict[str, Any]:
        """Bridge settings are ESPHome YAML / native entities."""
        raise _not_supported("Bridge settings")

    async def restart(self) -> dict[str, Any]:
        """Use ESPHome's own restart button."""
        raise _not_supported("Bridge restart")

    async def format_fs(self) -> dict[str, Any]:
        """The ESPHome firmware keeps no files."""
        raise _not_supported("Filesystem format")

    async def skip_next_clean(self) -> dict[str, Any]:
        """Use the native Skip next clean switch."""
        raise _not_supported("Skip next clean")

    async def cancel_skip_next_clean(self) -> dict[str, Any]:
        """Use the native Skip next clean switch."""
        raise _not_supported("Skip next clean")
