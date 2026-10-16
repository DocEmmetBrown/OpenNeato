"""End-to-end: a live clean on the ESPHome backend reaches the card and the mapper."""

from __future__ import annotations

import asyncio
from typing import Any
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.core import HomeAssistant

from custom_components.openneato import websocket
from custom_components.openneato.const import BACKEND_ESPHOME, CONF_BACKEND, CONF_DEVICE_ID, DOMAIN

from .conftest import MAC, FakeEsphomeDevice
from .test_esphome_history import HEADER, SID, PoseBuffer


@pytest.fixture(autouse=True)
def no_frontend():
    with patch("custom_components.openneato.async_register_card", AsyncMock()), patch(
        "custom_components.openneato._async_register_frontend", AsyncMock()
    ):
        yield


async def _ws(hass: HomeAssistant, handler, msg: dict[str, Any]) -> Any:
    connection = MagicMock()
    handler(hass, connection, {"id": 1, **msg})
    # async_response handlers run as background tasks, which
    # async_block_till_done does not always wait for: wait for the reply.
    for _ in range(100):
        await hass.async_block_till_done()
        if connection.send_result.called or connection.send_error.called:
            break
        await asyncio.sleep(0.01)
    if connection.send_error.called:
        raise AssertionError(connection.send_error.call_args)
    return connection.send_result.call_args[0][1]


async def test_live_clean(hass: HomeAssistant, esphome_device: FakeEsphomeDevice, tmp_path) -> None:
    """Poses recorded during a clean are served live, then as a finished replay."""
    hass.config.config_dir = str(tmp_path)
    buffer = PoseBuffer()
    esphome_device.handlers["get_pose_batch"] = buffer
    esphome_device.handlers["send_command"] = lambda data: {
        "response": "GetRobotPos Smooth\r\nRobot Smooth pose: X=1.000, Y=2.000, Theta=90.0, Time=100.0\r\n"
    }
    esphome_device.handlers["get_lidar"] = lambda data: {
        "rotationSpeed": 5.0,
        "validPoints": 360,
        "dist": [1500] * 360,
        "intensity": [100] * 360,
        "error": [0] * 360,
    }
    entry = MockConfigEntry(
        domain=DOMAIN, unique_id=MAC, data={CONF_BACKEND: BACKEND_ESPHOME, CONF_DEVICE_ID: esphome_device.device.id}
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    data = hass.data[DOMAIN][entry.entry_id]
    vacuum = websocket._vacuum_entity(hass, entry.entry_id)

    # The robot starts cleaning and the device records poses.
    buffer.current = {"id": SID, "active": True, "header": HEADER}
    for i in range(20):
        buffer.add(sid=SID, x=0.1 * (i + 1), y=0.2, t=0.0, ts=10.0 + 2 * i, b=1400)
    esphome_device.set("ui_state", "UIMGR_STATE_HOUSECLEANINGRUNNING")
    await data["recorder"].async_poll()
    await data["coordinator"].async_refresh()
    await hass.async_block_till_done()

    sessions = await _ws(hass, websocket.ws_list_sessions, {"type": "openneato/sessions", "entity_id": vacuum})
    live = next(s for s in sessions["sessions"] if s["name"] == f"{SID}.jsonl")
    assert live["recording"] is True
    session = await _ws(
        hass, websocket.ws_get_session, {"type": "openneato/session", "name": f"{SID}.jsonl", "entity_id": vacuum}
    )
    assert session["recording"] is True
    assert len(session["path"]) == 20 * 4
    assert session["coverage"]

    # The LIDAR mapper samples through the device's actions while cleaning.
    mapper = data["mapper"]
    assert mapper._collecting
    await mapper._async_tick()
    assert len(mapper._captures) == 1
    x, y, theta, points, _rpm = mapper._captures[0]
    assert (x, y, theta) == (1.0, 2.0, 90.0)
    assert len(points) == 360

    # The clean ends: the session becomes a finished replay with its summary.
    buffer.add(sid=SID, x=2.5, y=0.2, t=0.0, ts=60.0, b=1400)
    buffer.current = None
    buffer.previous = {"id": SID, "header": HEADER, "summary": {"type": "summary", "duration": 600}}
    esphome_device.set("ui_state", "UIMGR_STATE_IDLE")
    await data["recorder"].async_poll()
    await data["coordinator"].async_refresh()
    await hass.async_block_till_done()

    session = await _ws(
        hass, websocket.ws_get_session, {"type": "openneato/session", "name": f"{SID}.jsonl", "entity_id": vacuum}
    )
    assert session["recording"] is False
    assert len(session["path"]) == 21 * 4
    assert session["summary"]["duration"] == 600
    assert not mapper._collecting

    assert await hass.config_entries.async_unload(entry.entry_id)
