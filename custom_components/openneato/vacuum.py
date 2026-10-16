"""Vacuum entity for the OpenNeato integration."""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any

from homeassistant.components.vacuum import (
    StateVacuumEntity,
    VacuumActivity,
    VacuumEntityFeature,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import entity_platform
from homeassistant.helpers.entity_platform import AddEntitiesCallback
import voluptuous as vol

from .api import OpenNeatoApiClient
from .const import DOMAIN, FAN_SPEEDS, UISTATE_SUBSTRINGS
from .entity import OpenNeatoEntity
from .esphome_backend import OpenNeatoEsphomeClient

# vacuum.send_command commands that start a mapping run
MAPPING_RUN_COMMANDS = ("explore", "mapping_run", "map")
SERVICE_START_MAPPING_RUN = "start_mapping_run"
SERVICE_SAVE_FLOOR_PLAN = "save_floor_plan"
SERVICE_RESTORE_FLOOR_PLAN = "restore_floor_plan"
SERVICE_DELETE_FLOOR_PLAN = "delete_floor_plan"
SERVICE_RESET_FLOOR_PLAN = "reset_floor_plan"
SERVICE_MANUAL_MOVE = "manual_move"

# Persistent-map alerts: the robot refuses an exploration run without the
# Neato cloud (234 "Failed to load persistent map" and friends). The ESPHome
# firmware handles the fallback itself (neato_mapping.h); this is the same
# for the HTTP firmware.
# 236 (acquiring map data) is the attempt in progress, not a failure.
PERSISTENT_MAP_ALERTS = frozenset({231, 234, 235, 238, 239})
EXPLORE_TIMEOUT_S = 120.0
STOP_TIMEOUT_S = 120.0
FALLBACK_TIMEOUT_S = 6 * 3600.0
ROBOT_RESTART_S = 45.0
IDLE_STATES = ("UIMGR_STATE_IDLE", "UIMGR_STATE_STANDBY")
FALLBACK_POLL_S = 30.0
EXPLORE_POLL_S = 3.0
ATTR_NAME = "name"

_LOGGER = logging.getLogger(__name__)

SUPPORTED_FEATURES = (
    VacuumEntityFeature.START
    | VacuumEntityFeature.STOP
    | VacuumEntityFeature.PAUSE
    | VacuumEntityFeature.RETURN_HOME
    | VacuumEntityFeature.CLEAN_SPOT
    | VacuumEntityFeature.LOCATE
    | VacuumEntityFeature.FAN_SPEED
    | VacuumEntityFeature.SEND_COMMAND
    | VacuumEntityFeature.STATE
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up OpenNeato vacuum from a config entry."""
    data = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(
        [
            OpenNeatoVacuum(
                coordinator=data["coordinator"],
                api=data["api"],
                serial=data["serial"],
                model=data["model"],
                sw_version=data["sw_version"],
                fw_version=data["fw_version"],
                host=data["host"],
                via_device_id=data.get("via_device_id"),
            )
        ]
    )

    platform = entity_platform.async_get_current_platform()
    platform.async_register_entity_service(SERVICE_START_MAPPING_RUN, None, "async_start_mapping_run")
    platform.async_register_entity_service(
        SERVICE_SAVE_FLOOR_PLAN, {vol.Optional(ATTR_NAME): cv.string}, "async_save_floor_plan"
    )
    platform.async_register_entity_service(
        SERVICE_RESTORE_FLOOR_PLAN, {vol.Required(ATTR_NAME): cv.string}, "async_restore_floor_plan"
    )
    platform.async_register_entity_service(
        SERVICE_DELETE_FLOOR_PLAN, {vol.Required(ATTR_NAME): cv.string}, "async_delete_floor_plan"
    )
    platform.async_register_entity_service(SERVICE_RESET_FLOOR_PLAN, None, "async_reset_floor_plan")
    platform.async_register_entity_service(
        SERVICE_MANUAL_MOVE,
        {
            vol.Required("left_mm"): vol.All(vol.Coerce(int), vol.Range(min=-10000, max=10000)),
            vol.Required("right_mm"): vol.All(vol.Coerce(int), vol.Range(min=-10000, max=10000)),
            vol.Optional("speed", default=150): vol.All(vol.Coerce(int), vol.Range(min=0, max=350)),
        },
        "async_manual_move",
    )


class OpenNeatoVacuum(OpenNeatoEntity, StateVacuumEntity):
    """Representation of an OpenNeato vacuum cleaner."""

    _attr_supported_features = SUPPORTED_FEATURES
    _attr_fan_speed_list = FAN_SPEEDS
    _attr_name = None  # Use device name

    def __init__(
        self,
        coordinator,
        api: OpenNeatoApiClient,
        serial: str,
        model: str | None = None,
        sw_version: str | None = None,
        fw_version: str | None = None,
        host: str | None = None,
        via_device_id: str | None = None,
    ) -> None:
        """Initialize the vacuum entity."""
        super().__init__(
            coordinator, serial,
            model=model, sw_version=sw_version, fw_version=fw_version, host=host,
            via_device_id=via_device_id,
        )
        self._api = api
        self._attr_unique_id = f"{serial}_vacuum"

    @property
    def activity(self) -> VacuumActivity | None:
        """Return the current vacuum activity."""
        if not self.coordinator.data:
            return None

        state_data = self.coordinator.data.get("state", {})
        charger_data = self.coordinator.data.get("charger", {})
        error_data = self.coordinator.data.get("error", {})

        # A real error takes priority. The bridge reports the robot's alerts
        # (UI_ALERT 201-242: returning to base, recovering location...) the same
        # way, with kind "warning"; those say what the robot is doing, so the
        # activity below stands. Older firmware without kind counts as an error.
        if error_data.get("hasError") and error_data.get("kind", "error") != "warning":
            return VacuumActivity.ERROR

        ui_state = state_data.get("uiState", "")

        # Match using substrings, same as the firmware frontend
        for substring, activity in UISTATE_SUBSTRINGS:
            if substring in ui_state:
                return activity

        # For unmapped states, use charger to distinguish docked vs idle
        if charger_data.get("chargingActive") or charger_data.get("extPwrPresent"):
            return VacuumActivity.DOCKED

        return VacuumActivity.IDLE

    @property
    def fan_speed(self) -> str | None:
        """Return the current fan speed."""
        if not self.coordinator.data:
            return None
        settings = self.coordinator.data.get("user_settings", {})
        if settings.get("ecoMode"):
            return "eco"
        if settings.get("intenseClean"):
            return "intense"
        return "normal"

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        """Return extra state attributes."""
        attrs: dict[str, Any] = {}
        if not self.coordinator.data:
            return attrs

        state_data = self.coordinator.data.get("state", {})
        error_data = self.coordinator.data.get("error", {})

        if robot_state := state_data.get("robotState"):
            attrs["robot_state"] = robot_state
        if ui_state := state_data.get("uiState"):
            attrs["ui_state"] = ui_state

        if error_data.get("hasError"):
            attrs["error_message"] = error_data.get("displayMessage", "Unknown error")
            attrs["error_code"] = error_data.get("errorCode")

        return attrs

    # -- Commands ----------------------------------------------------------

    async def async_start(self, **kwargs: Any) -> None:
        """Start or resume house cleaning."""
        await self._api.clean("house")
        await self.coordinator.async_request_refresh()

    async def async_stop(self, **kwargs: Any) -> None:
        """Stop cleaning."""
        await self._api.clean("stop")
        await self.coordinator.async_request_refresh()

    async def async_pause(self, **kwargs: Any) -> None:
        """Pause cleaning."""
        await self._api.clean("pause")
        await self.coordinator.async_request_refresh()

    async def async_return_to_base(self, **kwargs: Any) -> None:
        """Return to dock."""
        await self._api.clean("dock")
        await self.coordinator.async_request_refresh()

    async def async_clean_spot(self, **kwargs: Any) -> None:
        """Start spot cleaning."""
        await self._api.clean("spot")
        await self.coordinator.async_request_refresh()

    async def async_locate(self, **kwargs: Any) -> None:
        """Locate the vacuum by playing an alert sound."""
        await self._api.play_sound(19)

    async def async_set_fan_speed(self, fan_speed: str, **kwargs: Any) -> None:
        """Set the fan speed."""
        if fan_speed == "eco":
            await self._api.set_user_setting("EcoMode", "ON")
            await self._api.set_user_setting("IntenseClean", "OFF")
        elif fan_speed == "intense":
            await self._api.set_user_setting("EcoMode", "OFF")
            await self._api.set_user_setting("IntenseClean", "ON")
        else:
            await self._api.set_user_setting("EcoMode", "OFF")
            await self._api.set_user_setting("IntenseClean", "OFF")
        await self.coordinator.async_request_refresh()

    async def async_send_command(
        self, command: str, params: dict[str, Any] | list[Any] | None = None, **kwargs: Any
    ) -> None:
        """Send a raw serial command to the robot ("explore" starts a mapping run)."""
        if command.strip().lower() in MAPPING_RUN_COMMANDS:
            await self.async_start_mapping_run()
            return
        await self._api.send_serial_command(command)
        await self.coordinator.async_request_refresh()

    # -- Mapping run and floor plans ----------------------------------------

    def _mapper(self):
        mapper = self.hass.data[DOMAIN][self.platform.config_entry.entry_id].get("mapper")
        if mapper is None:
            raise HomeAssistantError("The LIDAR floor plan is turned off in the integration's options")
        return mapper

    async def async_start_mapping_run(self) -> None:
        """Map the floor with a quiet house clean (Eco on), saved as a floor plan.

        The robot's own exploration run (Clean Explore) needs the Neato cloud:
        without it the robot refuses it and every clean after it until it is
        restarted, so it is never sent.
        """
        if self.activity not in (VacuumActivity.DOCKED, VacuumActivity.IDLE):
            raise HomeAssistantError("Stop the robot before starting a mapping run")
        mapper = self.hass.data[DOMAIN][self.platform.config_entry.entry_id].get("mapper")
        if mapper is not None:
            mapper.async_mapping_run_requested()
        if isinstance(self._api, OpenNeatoEsphomeClient):
            # The firmware runs it, with the persistent-map recovery (neato_mapping.h).
            await self._api.clean("explore")
        else:
            self.hass.async_create_background_task(self._async_quiet_mapping_run(), f"{DOMAIN} mapping run")
        await self.coordinator.async_request_refresh()

    def _ui(self) -> str:
        return ((self.coordinator.data or {}).get("state") or {}).get("uiState", "")

    async def _async_wait(self, done, timeout: float, poll: float) -> bool:
        """Poll the coordinator until done(ui, error) or timeout."""
        began = time.monotonic()
        while time.monotonic() - began < timeout:
            await asyncio.sleep(poll)
            await self.coordinator.async_request_refresh()
            if done(self._ui(), (self.coordinator.data or {}).get("error") or {}):
                return True
        return False

    async def _async_quiet_mapping_run(self) -> None:
        """HTTP firmware: Eco on, house clean, persistent-map recovery, Eco back."""
        eco_before = bool((self.coordinator.data or {}).get("user_settings", {}).get("ecoMode"))
        await self._api.set_user_setting("EcoMode", "ON")
        try:
            for attempt in range(2):
                await self._api.clean("house")
                refused = False

                def started(ui: str, error: dict[str, Any]) -> bool:
                    nonlocal refused
                    refused = bool(error.get("hasError")) and int(error.get("errorCode") or 0) in PERSISTENT_MAP_ALERTS
                    return refused or "CLEANINGRUNNING" in ui

                if not await self._async_wait(started, EXPLORE_TIMEOUT_S, EXPLORE_POLL_S) or not refused:
                    break
                # Stuck starting: stop (only the app event leaves that state),
                # clear, and restart the robot before trying once more.
                _LOGGER.warning("Mapping run refused (persistent map): restarting the robot")
                await self._api.clean("stop")
                await self._api.clear_errors()
                if attempt or not await self._async_wait(lambda ui, _e: ui in IDLE_STATES, STOP_TIMEOUT_S, EXPLORE_POLL_S):
                    _LOGGER.warning("Mapping run: the robot would not start the clean; giving up")
                    return
                await self._api.power("restart")
                await asyncio.sleep(ROBOT_RESTART_S)
                if not await self._async_wait(lambda ui, _e: ui in IDLE_STATES, STOP_TIMEOUT_S, EXPLORE_POLL_S):
                    return
            # Wait for the clean to end before putting Eco back.
            await self._async_wait(lambda ui, _e: ui in IDLE_STATES, FALLBACK_TIMEOUT_S, FALLBACK_POLL_S)
        finally:
            if not eco_before:
                await self._api.set_user_setting("EcoMode", "OFF")

    async def async_manual_move(self, left_mm: int, right_mm: int, speed: int = 150) -> None:
        """Drive the wheels in manual mode (ESPHome firmware only)."""
        if not isinstance(self._api, OpenNeatoEsphomeClient):
            raise HomeAssistantError("Manual drive from Home Assistant needs the OpenNeato ESPHome firmware")
        await self._api.call_action(
            "manual_move", {"left_mm": left_mm, "right_mm": right_mm, "speed": speed}, response=False
        )

    async def async_save_floor_plan(self, name: str | None = None) -> None:
        """Save the current floor plan."""
        await self._mapper().async_save_plan(name)

    async def async_restore_floor_plan(self, name: str) -> None:
        """Replace the floor plan with a saved one."""
        await self._mapper().async_restore_plan(name)

    async def async_delete_floor_plan(self, name: str) -> None:
        """Forget a saved floor plan."""
        await self._mapper().async_delete_plan(name)

    async def async_reset_floor_plan(self) -> None:
        """Start the floor plan again from scratch."""
        await self._mapper().async_reset_map()
