"""Config flow for OpenNeato integration."""

from __future__ import annotations

import logging
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.selector import (
    DeviceFilterSelectorConfig,
    DeviceSelector,
    DeviceSelectorConfig,
)
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import OpenNeatoApiClient, OpenNeatoConnectionError
from .const import (
    BACKEND_ESPHOME,
    BACKEND_HTTP,
    CONF_BACKEND,
    CONF_DEVICE_ID,
    CONF_HOST,
    DOMAIN,
)
from .esphome_backend import (
    ESPHOME_DOMAIN,
    ESPHOME_MANUFACTURER,
    ESPHOME_MODEL,
    OpenNeatoEsphomeClient,
    device_mac,
    is_openneato_esphome_device,
    openneato_esphome_devices,
)

_LOGGER = logging.getLogger(__name__)

STEP_USER_DATA_SCHEMA = vol.Schema(
    {
        vol.Required(CONF_HOST): str,
    }
)


class OpenNeatoConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle a config flow for OpenNeato."""

    VERSION = 1

    def __init__(self) -> None:
        """Initialize the flow."""
        self._device_id: str | None = None
        self._mac: str | None = None

    # ── User-initiated ───────────────────────────────────────────────

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Pick the firmware when an OpenNeato ESPHome device is around."""
        if self._unconfigured_esphome_devices():
            return self.async_show_menu(step_id="user", menu_options=["esphome", "http"])
        return await self.async_step_http(user_input)

    async def async_step_http(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Connect to the OpenNeato HTTP bridge firmware."""
        errors: dict[str, str] = {}

        if user_input is not None:
            host = user_input[CONF_HOST]
            session = async_get_clientsession(self.hass)
            api = OpenNeatoApiClient(host, session)

            try:
                firmware_info = await api.get_firmware_version()
                robot_info = await api.get_robot_version()
            except OpenNeatoConnectionError:
                errors["base"] = "cannot_connect"
            except Exception:  # noqa: BLE001 -- surface anything as "unknown"
                _LOGGER.exception("Unexpected exception during config flow")
                errors["base"] = "unknown"
            else:
                serial = robot_info.get("serialNumber", "")
                model = robot_info.get("modelName")
                software_version = robot_info.get("softwareVersion")
                firmware_version = firmware_info.get("version")

                if not serial:
                    errors["base"] = "unknown"
                    return self.async_show_form(
                        step_id="http",
                        data_schema=STEP_USER_DATA_SCHEMA,
                        errors=errors,
                    )

                await self.async_set_unique_id(serial)
                self._abort_if_unique_id_configured()

                return self.async_create_entry(
                    title=model or f"OpenNeato ({host})",
                    data={
                        CONF_BACKEND: BACKEND_HTTP,
                        CONF_HOST: host,
                        "serial": serial,
                        "model": model,
                        "firmware_version": firmware_version,
                        "software_version": software_version,
                    },
                )

        return self.async_show_form(
            step_id="http",
            data_schema=STEP_USER_DATA_SCHEMA,
            errors=errors,
        )

    async def async_step_esphome(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Pick an ESPHome device running the OpenNeato ESPHome firmware."""
        errors: dict[str, str] = {}
        if user_input is not None:
            device = dr.async_get(self.hass).async_get(user_input[CONF_DEVICE_ID])
            if device is None or not is_openneato_esphome_device(self.hass, device):
                errors["base"] = "not_openneato_esphome"
            else:
                return await self._async_create_esphome_entry(device)

        schema = vol.Schema(
            {
                vol.Required(CONF_DEVICE_ID): DeviceSelector(
                    DeviceSelectorConfig(
                        filter=DeviceFilterSelectorConfig(
                            integration=ESPHOME_DOMAIN,
                            manufacturer=ESPHOME_MANUFACTURER,
                            model=ESPHOME_MODEL,
                        )
                    )
                )
            }
        )
        return self.async_show_form(step_id="esphome", data_schema=schema, errors=errors)

    # ── Discovery ────────────────────────────────────────────────────

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """An OpenNeato ESPHome device announced itself (`project_name`)."""
        mac = discovery_info.properties.get("mac")
        if not mac:
            return self.async_abort(reason="not_openneato_esphome")
        self._mac = dr.format_mac(mac)
        await self.async_set_unique_id(self._mac)
        self._abort_if_unique_id_configured()
        self.context["title_placeholders"] = {
            "name": discovery_info.properties.get("friendly_name") or discovery_info.hostname.split(".")[0]
        }
        return await self.async_step_esphome_confirm()

    async def async_step_integration_discovery(
        self, discovery_info: dict[str, Any]
    ) -> ConfigFlowResult:
        """The ESPHome integration registered an OpenNeato device."""
        device = dr.async_get(self.hass).async_get(discovery_info[CONF_DEVICE_ID])
        if device is None or not is_openneato_esphome_device(self.hass, device):
            return self.async_abort(reason="not_openneato_esphome")
        mac = device_mac(device)
        await self.async_set_unique_id(mac or device.id)
        self._abort_if_unique_id_configured()
        self._mac = mac
        self._device_id = device.id
        self.context["title_placeholders"] = {"name": device.name_by_user or device.name or "OpenNeato"}
        return await self.async_step_esphome_confirm()

    async def async_step_esphome_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Confirm a discovered device; it must be adopted in ESPHome first."""
        device = self._find_device()
        errors: dict[str, str] = {}
        if user_input is not None:
            if device is not None:
                return await self._async_create_esphome_entry(device)
            errors["base"] = "esphome_not_set_up"
        return self.async_show_form(
            step_id="esphome_confirm",
            description_placeholders={
                "name": self.context.get("title_placeholders", {}).get("name", "OpenNeato"),
            },
            errors=errors,
        )

    # ── Helpers ──────────────────────────────────────────────────────

    def _configured_device_ids(self) -> set[str]:
        return {
            entry.data[CONF_DEVICE_ID]
            for entry in self._async_current_entries(include_ignore=False)
            if entry.data.get(CONF_DEVICE_ID)
        }

    def _unconfigured_esphome_devices(self) -> list[dr.DeviceEntry]:
        configured = self._configured_device_ids()
        return [d for d in openneato_esphome_devices(self.hass) if d.id not in configured]

    def _find_device(self) -> dr.DeviceEntry | None:
        registry = dr.async_get(self.hass)
        if self._device_id and (device := registry.async_get(self._device_id)):
            return device if is_openneato_esphome_device(self.hass, device) else None
        if self._mac:
            for device in openneato_esphome_devices(self.hass):
                if device_mac(device) == self._mac:
                    return device
        return None

    async def _async_create_esphome_entry(self, device: dr.DeviceEntry) -> ConfigFlowResult:
        mac = device_mac(device)
        await self.async_set_unique_id(mac or device.id, raise_on_progress=False)
        self._abort_if_unique_id_configured()
        if device.id in self._configured_device_ids():
            return self.async_abort(reason="already_configured")

        client = OpenNeatoEsphomeClient(self.hass, device.id)
        robot = await client.get_robot_version()
        serial = robot.get("serialNumber") or None
        data = {
            CONF_BACKEND: BACKEND_ESPHOME,
            CONF_DEVICE_ID: device.id,
            "serial": serial,
            "model": robot.get("modelName") or None,
        }

        # Same robot, previously flashed with the HTTP firmware: switch that
        # entry over so its vacuum entity (keyed on the serial) is kept.
        if serial and (old := self._http_entry_for(serial)) is not None:
            return self.async_update_reload_and_abort(
                old, unique_id=mac or device.id, data=data, reason="migrated_to_esphome"
            )

        title = device.name_by_user or device.name or data["model"] or "OpenNeato"
        return self.async_create_entry(title=title, data=data)

    def _http_entry_for(self, serial: str) -> ConfigEntry | None:
        for entry in self._async_current_entries(include_ignore=False):
            if entry.unique_id == serial and entry.data.get(CONF_BACKEND, BACKEND_HTTP) == BACKEND_HTTP:
                return entry
        return None
