"""Base entity for the OpenNeato integration."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity, DataUpdateCoordinator

from .const import DOMAIN


class OpenNeatoEntity(CoordinatorEntity):
    """Base class for OpenNeato entities."""

    _attr_has_entity_name = True

    def __init__(
        self,
        coordinator: DataUpdateCoordinator,
        serial: str,
        model: str | None = None,
        sw_version: str | None = None,
        fw_version: str | None = None,
        host: str | None = None,
        via_device_id: str | None = None,
    ) -> None:
        """Initialize the entity.

        `via_device_id` (ESPHome backend) links the robot to the ESPHome
        bridge device it is reached through. The entity cannot join that
        device: it belongs to the ESPHome config entry.
        """
        super().__init__(coordinator)
        self._serial = serial
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, serial)},
            manufacturer="OpenNeato",
            model=model,
            sw_version=sw_version or fw_version,
            configuration_url=f"http://{host}" if host else None,
        )
        # `via_device_id` arrived in Home Assistant 2026.10; older versions
        # only link through identifiers, which ESPHome devices do not have.
        if via_device_id and "via_device_id" in DeviceInfo.__optional_keys__:
            self._attr_device_info["via_device_id"] = via_device_id
