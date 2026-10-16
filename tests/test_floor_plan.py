"""Tests for the mapping run and saved floor plans."""

from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import entity_registry as er

from custom_components.openneato.const import BACKEND_ESPHOME, CONF_BACKEND, CONF_DEVICE_ID, DOMAIN
from custom_components.openneato.lidar_runner import MIN_CAPTURES, LidarMapRunner

from .conftest import MAC, FakeEsphomeDevice

SCAN = [(a, 1500) for a in range(0, 360, 2)]


def _coordinator(history: list | None = None) -> SimpleNamespace:
    return SimpleNamespace(data={"history": history or []}, async_add_listener=lambda cb: lambda: None)


async def _runner(hass: HomeAssistant, history: list | None = None) -> LidarMapRunner:
    runner = LidarMapRunner(hass, "e1", MagicMock(), _coordinator(history))
    await runner.async_load()
    return runner


async def _finish_run(runner: LidarMapRunner, session: str = "100.jsonl") -> MagicMock:
    """Run _finish on a full set of captures with the merge mocked out."""
    runner._session_name = session
    runner._collecting = True
    runner._captures = [(0.1 * i, 0.0, 0.0, SCAN, 5.0) for i in range(MIN_CAPTURES)]
    merge = MagicMock(return_value={"total_walls": 1, "sessions": 1})
    runner._map.merge_session = merge
    with patch(
        "custom_components.openneato.lidar_runner.build_session_grids",
        return_value=({(0, 0): 1}, {(0, 0)}),
    ):
        await runner._finish()
    return merge


async def test_save_restore_delete_reset(hass: HomeAssistant) -> None:
    runner = await _runner(hass)
    with pytest.raises(HomeAssistantError, match="no floor plan"):
        await runner.async_save_plan("Empty")

    runner._map.walls = {(1, 1): 4}
    runner._map.sessions = 3
    version = runner.map_version
    assert await runner.async_save_plan("Ground floor") == "Ground floor"
    assert runner.plan_names == ["Ground floor"]
    assert runner.active_plan == "Ground floor"

    await runner.async_reset_map()
    assert runner.sessions == 0 and not runner._map.walls
    assert runner.active_plan is None
    assert runner.map_version != version

    await runner.async_restore_plan("Ground floor")
    assert runner._map.walls == {(1, 1): 4}
    assert runner.sessions == 3
    assert runner.active_plan == "Ground floor"

    named = await runner.async_save_plan()
    assert named.startswith("Floor plan ")
    await runner.async_delete_plan("Ground floor")
    assert runner.plan_names == [named]
    with pytest.raises(HomeAssistantError):
        await runner.async_restore_plan("Ground floor")

    # Persisted across a reload.
    await runner._plans_store.async_save(runner._plans_data())
    again = await _runner(hass)
    assert again.plan_names == [named]
    assert again.active_plan == named


async def test_no_changes_while_cleaning(hass: HomeAssistant) -> None:
    runner = await _runner(hass)
    runner._map.walls = {(1, 1): 4}
    await runner.async_save_plan("A")
    runner._collecting = True
    with pytest.raises(HomeAssistantError, match="clean"):
        await runner.async_restore_plan("A")
    with pytest.raises(HomeAssistantError, match="clean"):
        await runner.async_reset_map()


async def test_frozen_plan_skips_ordinary_cleans(hass: HomeAssistant) -> None:
    runner = await _runner(hass)
    await runner.async_set_learning(False)
    merge = await _finish_run(runner)
    merge.assert_not_called()

    await runner.async_set_learning(True)
    merge = await _finish_run(runner)
    merge.assert_called_once()
    assert runner.plan_names == []


async def test_mapping_run_merges_and_saves(hass: HomeAssistant) -> None:
    """A mapping run is merged even when frozen, and saved straight away."""
    history = [{"name": "100.jsonl", "recording": False, "session": {"type": "session", "mode": "explore"}}]
    runner = await _runner(hass, history)
    await runner.async_set_learning(False)
    runner._map.walls = {(1, 1): 4}
    merge = await _finish_run(runner)
    merge.assert_called_once()
    assert len(runner.plan_names) == 1
    assert runner.plan_names[0].startswith("Mapping run ")
    assert runner.active_plan == runner.plan_names[0]


async def test_mapping_run_requested_through_home_assistant(hass: HomeAssistant) -> None:
    """The HTTP firmware cannot label the session; the request marks it instead."""
    runner = await _runner(hass, [{"name": "100.jsonl", "session": {"mode": "house"}}])
    await runner.async_set_learning(False)
    runner._map.walls = {(1, 1): 4}
    runner.async_mapping_run_requested()
    with patch.object(runner, "_start_timer"):
        runner._start()
    merge = await _finish_run(runner)
    merge.assert_called_once()
    assert runner.plan_names[0].startswith("Mapping run ")


@pytest.fixture
async def esphome_entry(hass: HomeAssistant, esphome_device: FakeEsphomeDevice, tmp_path):
    hass.config.config_dir = str(tmp_path)
    esphome_device.handlers["get_pose_batch"] = lambda data: {
        "boot": 1, "seq": 0, "first_seq": 1, "more": False, "current": None, "previous": None, "entries": []
    }
    entry = MockConfigEntry(
        domain=DOMAIN, unique_id=MAC, data={CONF_BACKEND: BACKEND_ESPHOME, CONF_DEVICE_ID: esphome_device.device.id}
    )
    entry.add_to_hass(hass)
    with patch("custom_components.openneato.async_register_card", AsyncMock()), patch(
        "custom_components.openneato._async_register_frontend", AsyncMock()
    ):
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
        yield entry
        await hass.config_entries.async_unload(entry.entry_id)


def _entity(hass: HomeAssistant, entry, unique_id: str) -> str:
    return er.async_get(hass).async_get_entity_id(
        unique_id.split(":")[0], DOMAIN, unique_id.split(":")[1]
    )


async def test_floor_plan_entities(hass: HomeAssistant, esphome_entry, esphome_device: FakeEsphomeDevice) -> None:
    """The ESPHome entry gets only the floor-plan select/buttons/switch."""
    entities = er.async_entries_for_config_entry(er.async_get(hass), esphome_entry.entry_id)
    assert sorted(e.unique_id.split("_", 1)[1] for e in entities) == [
        "floor_plan",
        "floor_plan_learning",
        "reset_floor_plan",
        "save_floor_plan",
        "schedule_calendar",
        "vacuum",
    ]
    mapper = hass.data[DOMAIN][esphome_entry.entry_id]["mapper"]
    mapper._map.walls = {(2, 2): 3}
    select = _entity(hass, esphome_entry, "select:OPS12345-ABC_floor_plan")
    save = _entity(hass, esphome_entry, "button:OPS12345-ABC_save_floor_plan")
    learning = _entity(hass, esphome_entry, "switch:OPS12345-ABC_floor_plan_learning")

    await hass.services.async_call("button", "press", {"entity_id": save}, blocking=True)
    state = hass.states.get(select)
    assert state.state.startswith("Floor plan ")
    assert state.attributes["options"] == [state.state]

    await hass.services.async_call("switch", "turn_off", {"entity_id": learning}, blocking=True)
    assert hass.states.get(learning).state == "off"
    assert mapper.learning is False

    mapper._map.walls = {}
    await hass.services.async_call(
        "select", "select_option", {"entity_id": select, "option": state.state}, blocking=True
    )
    assert mapper._map.walls == {(2, 2): 3}


async def test_mapping_run_and_services(hass: HomeAssistant, esphome_entry, esphome_device: FakeEsphomeDevice) -> None:
    vacuum = _entity(hass, esphome_entry, "vacuum:OPS12345-ABC_vacuum")
    mapper = hass.data[DOMAIN][esphome_entry.entry_id]["mapper"]

    await hass.services.async_call("vacuum", "send_command", {"entity_id": vacuum, "command": "explore"}, blocking=True)
    assert ("clean", {"action": "explore"}) in esphome_device.calls
    assert mapper._mapping_requested_at > 0

    mapper._map.walls = {(3, 3): 2}
    await hass.services.async_call(DOMAIN, "save_floor_plan", {"entity_id": vacuum, "name": "Upstairs"}, blocking=True)
    await hass.services.async_call(DOMAIN, "reset_floor_plan", {"entity_id": vacuum}, blocking=True)
    assert not mapper._map.walls
    await hass.services.async_call(DOMAIN, "restore_floor_plan", {"entity_id": vacuum, "name": "Upstairs"}, blocking=True)
    assert mapper._map.walls == {(3, 3): 2}
    await hass.services.async_call(DOMAIN, "delete_floor_plan", {"entity_id": vacuum, "name": "Upstairs"}, blocking=True)
    assert mapper.plan_names == []

    await hass.services.async_call(
        DOMAIN, "manual_move", {"entity_id": vacuum, "left_mm": -100, "right_mm": 100}, blocking=True
    )
    assert ("manual_move", {"left_mm": -100, "right_mm": 100, "speed": 150}) in esphome_device.calls

    # Not while the robot is out.
    esphome_device.set("ui_state", "UIMGR_STATE_HOUSECLEANINGRUNNING")
    await hass.data[DOMAIN][esphome_entry.entry_id]["coordinator"].async_refresh()
    with pytest.raises(HomeAssistantError, match="Stop the robot"):
        await hass.services.async_call(DOMAIN, "start_mapping_run", {"entity_id": vacuum}, blocking=True)


async def test_http_explore_falls_back_to_quiet_clean(hass: HomeAssistant) -> None:
    """HTTP firmware: a refused exploration becomes an Eco house clean, Eco restored after."""
    from custom_components.openneato.vacuum import OpenNeatoVacuum

    api = MagicMock()
    for name in ("send_serial_command", "clear_errors", "set_user_setting", "clean"):
        setattr(api, name, AsyncMock(return_value={"ok": True}))
    states = iter(
        [
            ("UIMGR_STATE_STARTHOUSECLEANING", {"hasError": True, "errorCode": 234}),
            ("UIMGR_STATE_HOUSECLEANINGRUNNING", {"hasError": False, "errorCode": 200}),
            ("UIMGR_STATE_IDLE", {"hasError": False, "errorCode": 200}),
        ]
    )
    coordinator = MagicMock()
    coordinator.data = {"state": {"uiState": "UIMGR_STATE_IDLE"}, "error": {}, "user_settings": {"ecoMode": False}}

    async def refresh() -> None:
        ui, error = next(states, ("UIMGR_STATE_IDLE", {}))
        coordinator.data = {**coordinator.data, "state": {"uiState": ui}, "error": error}

    coordinator.async_request_refresh = refresh
    vac = OpenNeatoVacuum(coordinator, api, "SER1")
    mapper = MagicMock()
    vac.hass = hass
    vac.platform = SimpleNamespace(config_entry=SimpleNamespace(entry_id="e1"))
    hass.data.setdefault(DOMAIN, {})["e1"] = {"mapper": mapper}

    async def fake_sleep(_s: float) -> None:
        if vac._api.clean.await_count:
            await refresh()

    with patch("custom_components.openneato.vacuum.asyncio.sleep", fake_sleep):
        await vac._async_explore_fallback()

    api.clear_errors.assert_awaited_once()
    api.clean.assert_awaited_once_with("house")
    mapper.async_mapping_run_requested.assert_called_once()
    assert [c.args for c in api.set_user_setting.await_args_list] == [("EcoMode", "ON"), ("EcoMode", "OFF")]
