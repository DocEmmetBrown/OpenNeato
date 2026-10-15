"""Robot event entity for the OpenNeato integration (HTTP firmware).

Replaces the bridge's ntfy notifications with a Home Assistant `event`
entity: it fires `cleaning_started`, `cleaning_done`, `docking_recharge`,
`returned_to_base`, `error` and `alert` from the coordinator's state
transitions, mirroring the firmware's NotificationManager. Pair it with the
`openneato/notify.yaml` blueprint to send them to any `notify` service.

The ESPHome firmware has a native `Robot event` entity with the same event
types, so this platform is only set up for the HTTP backend.
"""

from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

from homeassistant.components.event import EventEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .coordinator import latest_completed_session
from .entity import OpenNeatoEntity

EVENT_CLEANING_STARTED = "cleaning_started"
EVENT_CLEANING_DONE = "cleaning_done"
EVENT_DOCKING_RECHARGE = "docking_recharge"
EVENT_RETURNED_TO_BASE = "returned_to_base"
EVENT_ERROR = "error"
EVENT_ALERT = "alert"
EVENT_TYPES = [
    EVENT_CLEANING_STARTED,
    EVENT_CLEANING_DONE,
    EVENT_DOCKING_RECHARGE,
    EVENT_RETURNED_TO_BASE,
    EVENT_ERROR,
    EVENT_ALERT,
]

# A finished session's summary is only attached to `cleaning_done` when it is
# this fresh; older ones belong to a previous clean.
SUMMARY_MAX_AGE_S = 900

_IDLE_STATES = ("UIMGR_STATE_IDLE", "UIMGR_STATE_STANDBY")


@dataclass
class RobotEventTracker:
    """State machine turning state/error snapshots into robot events.

    Ported from firmware/src/notification_manager.cpp (MIT); the ESPHome
    component implements the same rules.
    """

    prev_ui: str = ""
    cleaning_before_dock: bool = False
    prev_has_error: bool = False
    prev_error_code: int = 0
    initialized: bool = False

    def update(self, ui: str, robot_state: str, error: dict[str, Any] | None) -> list[tuple[str, dict[str, Any]]]:
        """Feed one snapshot; return the (event_type, attributes) it causes."""
        events: list[tuple[str, dict[str, Any]]] = []
        if ui:
            if self.prev_ui:
                events.extend(self._state_events(ui, robot_state))
            self.prev_ui = ui
        if error is not None:
            has_error = bool(error.get("hasError"))
            code = int(error.get("errorCode") or 0)
            # The first snapshot only seeds the tracker, so a restart of Home
            # Assistant does not re-announce an error that is already showing.
            if self.initialized and has_error and (not self.prev_has_error or code != self.prev_error_code):
                kind = EVENT_ALERT if error.get("kind") == "warning" else EVENT_ERROR
                events.append(
                    (kind, {"message": error.get("displayMessage") or error.get("errorMessage") or "", "code": code})
                )
            self.prev_has_error = has_error
            self.prev_error_code = code
            self.initialized = True
        return events

    def _state_events(self, ui: str, robot_state: str) -> list[tuple[str, dict[str, Any]]]:
        prev = self.prev_ui
        was_cleaning = "CLEANINGRUNNING" in prev
        was_docking = "DOCKING" in prev
        is_cleaning = "CLEANINGRUNNING" in ui
        is_docking = "DOCKING" in ui
        is_suspended = "CLEANINGSUSPENDED" in ui
        is_idle = ui in _IDLE_STATES
        is_recharging = "Charging_Cleaning" in robot_state

        events: list[tuple[str, dict[str, Any]]] = []
        if was_cleaning and is_docking:
            self.cleaning_before_dock = True

        prev_in_clean = "CLEANING" in prev or "DOCKING" in prev
        if is_cleaning and not prev_in_clean:
            events.append((EVENT_CLEANING_STARTED, {"message": "Cleaning started"}))

        if is_docking and not was_docking and is_recharging:
            events.append((EVENT_DOCKING_RECHARGE, {"message": "Returning to base to recharge"}))

        docking_done = was_docking and self.cleaning_before_dock and not is_recharging
        suspended_done = "CLEANINGSUSPENDED" in prev and self.cleaning_before_dock
        if (was_cleaning or docking_done or suspended_done) and is_idle:
            events.append((EVENT_CLEANING_DONE, {"message": "Cleaning done"}))
        if was_docking and is_idle and not is_recharging:
            events.append((EVENT_RETURNED_TO_BASE, {"message": "Returned to base"}))

        if was_docking and not is_docking and not is_suspended:
            self.cleaning_before_dock = False
        if is_idle:
            self.cleaning_before_dock = False
        return events


def done_message(summary: dict[str, Any] | None) -> tuple[str, dict[str, Any]]:
    """`Cleaning done` text and attributes, with stats like the ntfy message."""
    if not summary:
        return "Cleaning done", {}
    attrs: dict[str, Any] = {
        "duration": summary.get("duration"),
        "area": summary.get("areaCovered"),
        "distance": summary.get("distanceTraveled"),
        "recharges": summary.get("recharges"),
        "battery_start": summary.get("batteryStart"),
        "battery_end": summary.get("batteryEnd"),
    }
    attrs = {k: v for k, v in attrs.items() if v is not None}
    parts = []
    if isinstance(attrs.get("duration"), (int, float)):
        parts.append(f"{int(attrs['duration']) // 60}min")
    if isinstance(attrs.get("area"), (int, float)):
        parts.append(f"{attrs['area']:.1f}m²")
    if isinstance(attrs.get("distance"), (int, float)):
        parts.append(f"{attrs['distance']:.0f}m")
    start, end = attrs.get("battery_start"), attrs.get("battery_end")
    if isinstance(start, int) and isinstance(end, int) and start >= 0 and end >= 0:
        parts.append(f"{start}% → {end}%")
    message = "Cleaning done" + ("\n" + " | ".join(parts) if parts else "")
    return message, attrs


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up the robot event entity."""
    data = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(
        [
            OpenNeatoRobotEvent(
                coordinator=data["coordinator"],
                serial=data["serial"],
                model=data["model"],
                sw_version=data["sw_version"],
                fw_version=data["fw_version"],
                host=data["host"],
            )
        ]
    )


class OpenNeatoRobotEvent(OpenNeatoEntity, EventEntity):
    """Fires an event on cleaning/docking/error transitions."""

    _attr_translation_key = "robot_event"
    _attr_name = "Robot event"
    _attr_event_types = EVENT_TYPES

    def __init__(self, coordinator, serial: str, **kwargs: Any) -> None:
        """Initialize the event entity."""
        super().__init__(coordinator, serial, **kwargs)
        self._attr_unique_id = f"{serial}_robot_event"
        self._tracker = RobotEventTracker()

    async def async_added_to_hass(self) -> None:
        """Seed the tracker so existing state does not fire on startup."""
        await super().async_added_to_hass()
        self._feed(fire=False)

    @callback
    def _handle_coordinator_update(self) -> None:
        self._feed(fire=True)
        super()._handle_coordinator_update()

    @callback
    def _feed(self, *, fire: bool) -> None:
        data = self.coordinator.data or {}
        state = data.get("state") or {}
        error = data.get("error") if isinstance(data.get("error"), dict) and data.get("error") else None
        events = self._tracker.update(state.get("uiState", ""), state.get("robotState", ""), error)
        if not fire:
            return
        for event_type, attrs in events:
            if event_type == EVENT_CLEANING_DONE:
                summary = self._fresh_summary(data.get("history"))
                message, stats = done_message(summary)
                attrs = {**attrs, **stats, "message": message}
            self._trigger_event(event_type, attrs)
            self.async_write_ha_state()

    @staticmethod
    def _fresh_summary(history: Any) -> dict[str, Any] | None:
        session = latest_completed_session(history)
        summary = (session or {}).get("summary")
        if not isinstance(summary, dict):
            return None
        ended = summary.get("time")
        if isinstance(ended, (int, float)) and time.time() - ended > SUMMARY_MAX_AGE_S:
            return None
        return summary
