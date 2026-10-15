"""Tests for the OpenNeato config flow."""

from __future__ import annotations

from ipaddress import ip_address
from unittest.mock import AsyncMock, patch

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from custom_components.openneato.const import BACKEND_ESPHOME, CONF_BACKEND, CONF_DEVICE_ID, DOMAIN

from .conftest import MAC, FakeEsphomeDevice

ZEROCONF = ZeroconfServiceInfo(
    ip_address=ip_address("192.168.1.50"),
    ip_addresses=[ip_address("192.168.1.50")],
    hostname="neato-c3.local.",
    name="neato-c3._esphomelib._tcp.local.",
    port=6053,
    type="_esphomelib._tcp.local.",
    properties={"mac": MAC.replace(":", ""), "project_name": "openneato.esphome", "friendly_name": "Neato"},
)


@pytest.fixture(autouse=True)
def no_setup(hass: HomeAssistant):
    """Do not set entries up while testing the flow."""
    with patch("custom_components.openneato.async_setup_entry", AsyncMock(return_value=True)), patch(
        "custom_components.openneato.async_setup", AsyncMock(return_value=True)
    ):
        yield
    # Never really set up, so there is nothing to unload at teardown.
    for entry in hass.config_entries.async_entries(DOMAIN):
        entry._async_set_state(hass, config_entries.ConfigEntryState.NOT_LOADED, None)


async def test_user_without_esphome_goes_to_http(hass: HomeAssistant) -> None:
    """No OpenNeato ESPHome device: the host form, as before."""
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "http"


async def test_http_flow(hass: HomeAssistant) -> None:
    """The HTTP firmware path still creates a host entry."""
    with patch(
        "custom_components.openneato.config_flow.OpenNeatoApiClient.get_firmware_version",
        AsyncMock(return_value={"version": "1.14.1"}),
    ), patch(
        "custom_components.openneato.config_flow.OpenNeatoApiClient.get_robot_version",
        AsyncMock(return_value={"serialNumber": "SER1", "modelName": "BotVacD7Connected"}),
    ):
        result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
        result = await hass.config_entries.flow.async_configure(result["flow_id"], {"host": "10.0.0.2"})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"]["host"] == "10.0.0.2"
    assert result["result"].unique_id == "SER1"


async def test_user_esphome(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """With an OpenNeato ESPHome device around, the user picks the backend."""
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert result["type"] is FlowResultType.MENU
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"next_step_id": "esphome"})
    assert result["step_id"] == "esphome"
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_DEVICE_ID: esphome_device.device.id}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"] == {
        CONF_BACKEND: BACKEND_ESPHOME,
        CONF_DEVICE_ID: esphome_device.device.id,
        "serial": "OPS12345-ABC",
        "model": "BotVacD7Connected",
    }
    assert result["result"].unique_id == MAC


async def test_migrates_http_entry(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """Flashing ESPHome on a robot set up over HTTP switches the old entry."""
    old = MockConfigEntry(domain=DOMAIN, unique_id="OPS12345-ABC", data={"host": "10.0.0.2", "serial": "OPS12345-ABC"})
    old.add_to_hass(hass)
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"next_step_id": "esphome"})
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_DEVICE_ID: esphome_device.device.id}
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "migrated_to_esphome"
    await hass.async_block_till_done()
    assert old.unique_id == MAC
    assert old.data[CONF_BACKEND] == BACKEND_ESPHOME
    assert old.data[CONF_DEVICE_ID] == esphome_device.device.id


async def test_zeroconf(hass: HomeAssistant, esphome_device: FakeEsphomeDevice) -> None:
    """The ESPHome mDNS announcement (project_name) starts a flow."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=ZEROCONF
    )
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "esphome_confirm"
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"][CONF_DEVICE_ID] == esphome_device.device.id

    # Already configured now.
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=ZEROCONF
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"


async def test_zeroconf_before_esphome_adoption(hass: HomeAssistant) -> None:
    """The device must be adopted in ESPHome before it can be confirmed."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=ZEROCONF
    )
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "esphome_not_set_up"}


async def test_integration_discovery_dedupes_with_zeroconf(
    hass: HomeAssistant, esphome_device: FakeEsphomeDevice
) -> None:
    """Zeroconf and device-registry discovery share the MAC unique_id."""
    await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=ZEROCONF)
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_INTEGRATION_DISCOVERY},
        data={CONF_DEVICE_ID: esphome_device.device.id},
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_in_progress"
