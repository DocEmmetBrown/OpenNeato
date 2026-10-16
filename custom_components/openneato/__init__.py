"""The OpenNeato integration."""

from __future__ import annotations

import logging
from typing import Any

from homeassistant.config_entries import SOURCE_INTEGRATION_DISCOVERY, ConfigEntry
from homeassistant.const import EVENT_HOMEASSISTANT_STARTED, Platform
from homeassistant.core import Event, HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import discovery_flow
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.typing import ConfigType

from . import websocket
from .api import OpenNeatoApiClient, OpenNeatoConnectionError
from .card import async_register_card, async_remove_card_resource
from .const import (
    BACKEND_ESPHOME,
    CONF_BACKEND,
    CONF_DEVICE_ID,
    CONF_HOST,
    CONF_MAP_ENABLED,
    DOMAIN,
    FORK_MIN_VERSION,
    FORK_RELEASES_URL,
    MAP_DEFAULT_ENABLED,
)
from .coordinator import OpenNeatoCoordinator
from .esphome_backend import (
    OpenNeatoEsphomeClient,
    is_openneato_esphome_device,
    openneato_esphome_devices,
)
from .esphome_history import EsphomeSessionRecorder
from .firmware_check import FIRMWARE_UNSUPPORTED, classify_firmware
from .floor_plan import FLOOR_PLAN_KEYS
from .http import OpenNeatoMapView
from .lidar_runner import LidarMapRunner

_LOGGER = logging.getLogger(__name__)

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

_FRONTEND_REGISTERED = "frontend_registered"

PLATFORMS: list[Platform] = [
    Platform.VACUUM,
    Platform.SENSOR,
    Platform.BINARY_SENSOR,
    Platform.SWITCH,
    Platform.NUMBER,
    Platform.BUTTON,
    # Platform.CAMERA is deliberately absent. The "LIDAR map" and "Cleaning
    # replay" cameras were server-rendered PNG/GIF snapshots; the canvas card
    # in www/openneato-replay-card.js supersedes both, drawing from the
    # openneato/session(s) websocket API with pan, zoom and scrubbing.
    #
    # camera.py, lidar_renderer.py and history_renderer.py were deleted with
    # it (1347 lines). The one piece still needed, _try_repair_pose, now lives
    # in replay.py.
    Platform.SELECT,
    Platform.TEXT,
    Platform.TIME,
    Platform.EVENT,
]
# On the ESPHome firmware every sensor, switch, number, select, button and the
# robot event are native ESPHome entities; the integration only adds what
# ESPHome cannot express: the vacuum entity, the schedule as a calendar, the
# replay card's history and the LIDAR floor plan (with its select, buttons and
# switch; those platforms add nothing else on this backend).
ESPHOME_PLATFORMS: list[Platform] = [
    Platform.VACUUM,
    Platform.CALENDAR,
    Platform.SELECT,
    Platform.BUTTON,
    Platform.SWITCH,
]
# Unique-id suffixes ("<serial>_<suffix>") of the entities this integration
# creates on the ESPHome backend.
ESPHOME_ENTITY_KEYS = ("vacuum", "schedule_calendar", *FLOOR_PLAN_KEYS)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    """Set up OpenNeato: serve the replay card to the dashboards.

    The card ships inside the integration rather than as a separate HACS
    resource, so the card and the WebSocket API it talks to can never fall out
    of version sync. It is registered here, not per config entry, so it is on
    the dashboards at every start even while the robot is offline.
    """
    await async_register_card(hass)
    _async_setup_esphome_discovery(hass)
    return True


@callback
def _async_setup_esphome_discovery(hass: HomeAssistant) -> None:
    """Offer a config flow for every OpenNeato ESPHome device.

    ESPHome devices are adopted by the ESPHome integration; the device it
    creates reports manufacturer `openneato` / model `esphome` (from the
    firmware's `project: name:`), which is what is matched here.
    """

    @callback
    def _discover(device: dr.DeviceEntry) -> None:
        if not is_openneato_esphome_device(hass, device):
            return
        if any(
            entry.data.get(CONF_DEVICE_ID) == device.id
            for entry in hass.config_entries.async_entries(DOMAIN, include_ignore=False)
        ):
            return
        discovery_flow.async_create_flow(
                hass,
                DOMAIN,
                context={"source": SOURCE_INTEGRATION_DISCOVERY},
                data={CONF_DEVICE_ID: device.id},
            )

    @callback
    def _discover_all(_event: Event | None = None) -> None:
        for device in openneato_esphome_devices(hass):
            _discover(device)

    @callback
    def _device_updated(event: Event[dr.EventDeviceRegistryUpdatedData]) -> None:
        if event.data["action"] not in ("create", "update"):
            return
        if (device := dr.async_get(hass).async_get(event.data["device_id"])) is not None:
            _discover(device)

    hass.bus.async_listen(dr.EVENT_DEVICE_REGISTRY_UPDATED, _device_updated)
    if hass.is_running:
        _discover_all()
    else:
        hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STARTED, _discover_all)


def _entry_platforms(entry: ConfigEntry) -> list[Platform]:
    if entry.data.get(CONF_BACKEND) == BACKEND_ESPHOME:
        return ESPHOME_PLATFORMS
    return PLATFORMS


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up OpenNeato from a config entry."""
    if entry.data.get(CONF_BACKEND) == BACKEND_ESPHOME:
        return await _async_setup_esphome_entry(hass, entry)
    host = entry.data[CONF_HOST]
    session = async_get_clientsession(hass)
    api = OpenNeatoApiClient(host, session)

    _LOGGER.debug("Connecting to OpenNeato at %s", host)
    try:
        firmware_info = await api.get_firmware_version()
        robot_info = await api.get_robot_version()
    except OpenNeatoConnectionError as err:
        _LOGGER.warning("Cannot connect to OpenNeato at %s: %s", host, err)
        raise ConfigEntryNotReady(
            f"Cannot connect to OpenNeato at {host}: {err}"
        ) from err
    except Exception as err:
        _LOGGER.exception("Unexpected error connecting to OpenNeato at %s", host)
        raise ConfigEntryNotReady(
            f"Unexpected error connecting to OpenNeato at {host}: {err}"
        ) from err

    _LOGGER.info(
        "Connected to OpenNeato at %s — %s (%s) firmware %s",
        host,
        robot_info.get("modelName"),
        robot_info.get("serialNumber"),
        firmware_info.get("version"),
    )

    # Upstream or pre-fork firmware keeps working (the coordinator drops the
    # endpoints it 404s on) but gets a repair issue pointing at the fork
    # build, since that is what the entities and the card are written for.
    issue_id = f"unsupported_firmware_{entry.entry_id}"
    if classify_firmware(firmware_info) == FIRMWARE_UNSUPPORTED:
        ir.async_create_issue(
            hass,
            DOMAIN,
            issue_id,
            is_fixable=False,
            severity=ir.IssueSeverity.WARNING,
            translation_key="unsupported_firmware",
            translation_placeholders={
                "host": host,
                "version": str(firmware_info.get("version", "?")),
                "repository": str(firmware_info.get("repositoryUrl", "?")),
                "min_version": ".".join(str(part) for part in FORK_MIN_VERSION),
                "releases_url": FORK_RELEASES_URL,
            },
        )
    else:
        ir.async_delete_issue(hass, DOMAIN, issue_id)

    serial = robot_info.get("serialNumber", entry.data.get("serial", "unknown"))
    model = robot_info.get("modelName", entry.data.get("model"))
    sw_version = robot_info.get("softwareVersion", entry.data.get("software_version"))
    fw_version = firmware_info.get("version", entry.data.get("firmware_version"))

    coordinator = OpenNeatoCoordinator(hass, api)
    await coordinator.async_config_entry_first_refresh()

    await _async_finish_setup(
        hass,
        entry,
        api,
        coordinator,
        {
            "serial": serial,
            "model": model,
            "sw_version": sw_version,
            "fw_version": fw_version,
            "host": host,
        },
    )
    return True


async def _async_setup_esphome_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up a robot running the OpenNeato ESPHome firmware."""
    client = OpenNeatoEsphomeClient(hass, entry.data[CONF_DEVICE_ID])
    if client.device is None:
        raise ConfigEntryNotReady("The OpenNeato ESPHome device is not in the device registry")
    if not client.available():
        raise ConfigEntryNotReady("The OpenNeato ESPHome device is not connected")

    recorder = EsphomeSessionRecorder(hass, entry.entry_id, client)
    await recorder.async_load()
    client.recorder = recorder

    coordinator = OpenNeatoCoordinator(hass, client)
    await coordinator.async_config_entry_first_refresh()

    robot_info = await client.get_robot_version()
    firmware_info = await client.get_firmware_version()
    # Entity unique IDs are keyed on the robot serial (as with the HTTP
    # firmware, so a migrated entry keeps its vacuum entity); wait for the
    # robot to report it rather than inventing a key that changes later.
    serial = entry.data.get("serial") or robot_info.get("serialNumber")
    if not serial:
        raise ConfigEntryNotReady("The robot has not reported its serial number yet")
    if entry.data.get("serial") != serial:
        hass.config_entries.async_update_entry(entry, data={**entry.data, "serial": serial})

    # An entry migrated from the HTTP firmware still owns the entities that
    # ESPHome now provides natively; drop them.
    keep = {f"{serial}_{key}" for key in ESPHOME_ENTITY_KEYS}
    registry = er.async_get(hass)
    for entity in er.async_entries_for_config_entry(registry, entry.entry_id):
        if entity.unique_id not in keep:
            registry.async_remove(entity.entity_id)

    await _async_finish_setup(
        hass,
        entry,
        client,
        coordinator,
        {
            "serial": serial,
            "model": robot_info.get("modelName") or entry.data.get("model"),
            "sw_version": robot_info.get("softwareVersion"),
            "fw_version": firmware_info.get("version"),
            "host": None,
            "via_device_id": client.device_id,
            "recorder": recorder,
        },
    )

    # Re-read the history section as soon as a session starts or ends so the
    # card and the last-clean sensors do not wait for the slow cadence.
    entry.async_on_unload(recorder.async_add_listener(_refresh_later(hass, coordinator)))
    recorder.async_start()
    entry.async_on_unload(recorder.async_stop)
    return True


def _refresh_later(hass: HomeAssistant, coordinator: OpenNeatoCoordinator):
    @callback
    def _refresh() -> None:
        hass.async_create_task(coordinator.async_request_refresh())

    return _refresh


async def _async_finish_setup(
    hass: HomeAssistant,
    entry: ConfigEntry,
    api: Any,
    coordinator: OpenNeatoCoordinator,
    info: dict[str, Any],
) -> None:
    """Shared tail of the HTTP and ESPHome setups."""
    hass.data.setdefault(DOMAIN, {})
    await _async_register_frontend(hass)

    # Builds a floor plan from the robot's own LIDAR, one cleaning at a time.
    # The accumulated map lives in its own store so it survives restarts and
    # keeps improving instead of being rebuilt from a single run.
    mapper = None
    if entry.options.get(CONF_MAP_ENABLED, MAP_DEFAULT_ENABLED):
        mapper = LidarMapRunner(hass, entry.entry_id, api, coordinator)
        await mapper.async_load()

    hass.data[DOMAIN][entry.entry_id] = {
        "api": api,
        "coordinator": coordinator,
        "mapper": mapper,
        **info,
    }

    await hass.config_entries.async_forward_entry_setups(entry, _entry_platforms(entry))

    # Reload the entry when its options change (e.g. floorplan background
    # calibration) so camera entities pick up the new config without a
    # full HA restart.
    entry.async_on_unload(entry.add_update_listener(async_reload_options))


async def _async_register_frontend(hass: HomeAssistant) -> None:
    """Register the API the replay card calls.

    Runs once per HA start, not once per config entry — the WebSocket commands
    and the map view are global.
    """
    if hass.data[DOMAIN].get(_FRONTEND_REGISTERED):
        return
    hass.data[DOMAIN][_FRONTEND_REGISTERED] = True

    websocket.async_register(hass)
    hass.http.register_view(OpenNeatoMapView())


async def async_reload_options(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Reload the config entry to apply updated options."""
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload an OpenNeato config entry."""
    if unload_ok := await hass.config_entries.async_unload_platforms(
        entry, _entry_platforms(entry)
    ):
        stored = hass.data[DOMAIN].pop(entry.entry_id)
        if stored.get("mapper"):
            stored["mapper"].async_unload()
        # Drop this entry's parsed replay sessions so a reload re-fetches
        # instead of serving a stale map from before a recalibration.
        for cache_key in (websocket.CACHE_KEY, websocket.LIVE_CACHE_KEY):
            cache = hass.data[DOMAIN].get(cache_key)
            if isinstance(cache, dict):
                for key in [k for k in cache if k[0] == entry.entry_id]:
                    cache.pop(key, None)
    return unload_ok


async def async_remove_entry(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Drop recorded sessions; remove the card with the last OpenNeato robot."""
    if entry.data.get(CONF_BACKEND) == BACKEND_ESPHOME:
        client = OpenNeatoEsphomeClient(hass, entry.data[CONF_DEVICE_ID])
        await EsphomeSessionRecorder(hass, entry.entry_id, client).async_remove_all()
    others = [
        other
        for other in hass.config_entries.async_entries(DOMAIN)
        if other.entry_id != entry.entry_id
    ]
    if not others:
        await async_remove_card_resource(hass)
