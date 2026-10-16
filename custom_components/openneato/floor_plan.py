"""Floor-plan entities: pick, save, reset and freeze the LIDAR floor plan.

The plan is the map the integration builds from the robot's LIDAR
(lidar_runner.py). A mapping run (a quiet house clean) is saved automatically;
these entities let the plan be kept as it is (stop learning from ordinary
cleans), saved under a name, restored later or started again from scratch.
"""

from __future__ import annotations

from typing import Any

from homeassistant.components.button import ButtonEntity
from homeassistant.components.select import SelectEntity
from homeassistant.components.switch import SwitchEntity
from homeassistant.const import EntityCategory

from .entity import OpenNeatoEntity
from .lidar_runner import LidarMapRunner

# Unique-id suffixes (after "<serial>_"), also kept when an HTTP entry is
# migrated to the ESPHome firmware.
FLOOR_PLAN_KEYS = ("floor_plan", "save_floor_plan", "reset_floor_plan", "floor_plan_learning")


def _device_kwargs(data: dict[str, Any]) -> dict[str, Any]:
    return {
        "model": data["model"],
        "sw_version": data["sw_version"],
        "fw_version": data["fw_version"],
        "host": data.get("host"),
        "via_device_id": data.get("via_device_id"),
    }


class _FloorPlanEntity(OpenNeatoEntity):
    """Base: follows the runner's plan changes."""

    _key: str

    def __init__(self, data: dict[str, Any]) -> None:
        super().__init__(data["coordinator"], data["serial"], **_device_kwargs(data))
        self._runner: LidarMapRunner = data["mapper"]
        self._attr_unique_id = f"{data['serial']}_{self._key}"
        self._attr_translation_key = self._key

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(self._runner.async_add_plan_listener(self.async_write_ha_state))


class FloorPlanSelect(_FloorPlanEntity, SelectEntity):
    """The saved floor plans; picking one restores it."""

    _key = "floor_plan"
    _attr_name = "Floor plan"
    _attr_icon = "mdi:floor-plan"

    @property
    def options(self) -> list[str]:
        return self._runner.plan_names

    @property
    def current_option(self) -> str | None:
        return self._runner.active_plan

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {"cleanings_merged": self._runner.sessions, "learning": self._runner.learning}

    async def async_select_option(self, option: str) -> None:
        await self._runner.async_restore_plan(option)


class SaveFloorPlanButton(_FloorPlanEntity, ButtonEntity):
    """Save the current plan, named after the date and time."""

    _key = "save_floor_plan"
    _attr_name = "Save floor plan"
    _attr_icon = "mdi:content-save-outline"

    async def async_press(self) -> None:
        await self._runner.async_save_plan()


class ResetFloorPlanButton(_FloorPlanEntity, ButtonEntity):
    """Start the plan again from scratch (saved plans are kept)."""

    _key = "reset_floor_plan"
    _attr_name = "Reset floor plan"
    _attr_icon = "mdi:map-marker-off-outline"
    _attr_entity_category = EntityCategory.CONFIG

    async def async_press(self) -> None:
        await self._runner.async_reset_map()


class FloorPlanLearningSwitch(_FloorPlanEntity, SwitchEntity):
    """Whether ordinary cleans keep refining the plan; mapping runs always do."""

    _key = "floor_plan_learning"
    _attr_name = "Learn floor plan from cleans"
    _attr_icon = "mdi:map-plus"
    _attr_entity_category = EntityCategory.CONFIG

    @property
    def is_on(self) -> bool:
        return self._runner.learning

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self._runner.async_set_learning(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._runner.async_set_learning(False)


def floor_plan_entities(data: dict[str, Any], kinds: tuple[type, ...]) -> list[Any]:
    """The floor-plan entities of the given classes, when the map is enabled."""
    if data.get("mapper") is None:
        return []
    return [cls(data) for cls in kinds]
