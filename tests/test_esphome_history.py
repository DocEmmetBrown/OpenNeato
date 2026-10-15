"""Tests for the ESPHome cleaning-history recorder."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from homeassistant.core import HomeAssistant

from custom_components.openneato.api import OpenNeatoApiError
from custom_components.openneato.esphome_backend import OpenNeatoEsphomeClient
from custom_components.openneato.esphome_history import EsphomeSessionRecorder
from custom_components.openneato.replay import build_replay_session

from .conftest import FakeEsphomeDevice

SID = 1771683615
HEADER = {"type": "session", "mode": "house", "time": SID, "battery": 98}
SUMMARY = {
    "type": "summary",
    "time": SID + 600,
    "duration": 600,
    "mode": "house",
    "recharges": 1,
    "snapshots": 300,
    "distanceTraveled": 41.5,
    "maxDistFromOrigin": 4.1,
    "totalRotation": 5200.0,
    "areaCovered": 12.5,
    "errorsDuringClean": 0,
    "batteryStart": 98,
    "batteryEnd": 61,
}


class PoseBuffer:
    """Device-side ring buffer serving get_pose_batch like the firmware."""

    def __init__(self, boot: int = 1, capacity: int = 256) -> None:
        self.boot = boot
        self.capacity = capacity
        self.entries: list[dict[str, Any]] = []
        self.seq = 0
        self.current: dict[str, Any] | None = None
        self.previous: dict[str, Any] | None = None
        self.requests: list[int] = []

    def add(self, **entry: Any) -> None:
        self.seq += 1
        self.entries.append({"seq": self.seq, **entry})
        self.entries = self.entries[-self.capacity :]

    def __call__(self, data: dict[str, Any]) -> dict[str, Any]:
        since = int(data["since_seq"])
        self.requests.append(since)
        newer = [e for e in self.entries if e["seq"] > since]
        batch = newer[:100]
        return {
            "boot": self.boot,
            "seq": self.seq,
            "first_seq": self.entries[0]["seq"] if self.entries else self.seq + 1,
            "more": len(newer) > len(batch),
            "current": self.current,
            "previous": self.previous,
            "entries": batch,
        }


@pytest.fixture
def buffer(esphome_device: FakeEsphomeDevice) -> PoseBuffer:
    buf = PoseBuffer()
    esphome_device.handlers["get_pose_batch"] = buf
    return buf


@pytest.fixture
async def recorder(hass: HomeAssistant, esphome_device: FakeEsphomeDevice, tmp_path: Path) -> EsphomeSessionRecorder:
    client = OpenNeatoEsphomeClient(hass, esphome_device.device.id)
    rec = EsphomeSessionRecorder(hass, "entry1", client, base_dir=str(tmp_path))
    client.recorder = rec
    await rec.async_load()
    return rec


async def test_records_a_session(hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer) -> None:
    """Poses, recharges and the summary end up in replayable JSONL."""
    buffer.current = {"id": SID, "active": True, "header": HEADER}
    for i in range(150):
        buffer.add(sid=SID, x=0.01 * (i + 1), y=0.5, t=90.0, ts=100.0 + 2 * i, b=1400)
    await recorder.async_poll()

    # 150 entries need two batches of <= 100.
    assert buffer.requests == [0, 100]
    sessions = recorder.list_sessions()
    assert len(sessions) == 1
    assert sessions[0]["name"] == f"{SID}.jsonl"
    assert sessions[0]["recording"] is True
    assert sessions[0]["session"]["battery"] == 98
    size_mid = sessions[0]["size"]
    assert size_mid > 150 * 30

    buffer.add(sid=SID, type="recharge", x=1.5, y=0.5)
    buffer.add(sid=SID, x=1.6, y=0.5, t=91.0, ts=500.0)
    buffer.current = None
    buffer.previous = {"id": SID, "header": HEADER, "summary": SUMMARY}
    await recorder.async_poll()

    sessions = recorder.list_sessions()
    assert sessions[0]["recording"] is False
    assert sessions[0]["summary"]["areaCovered"] == 12.5
    assert sessions[0]["size"] > size_mid

    raw = await recorder.async_read_session(f"{SID}.jsonl")
    lines = raw.strip().split("\n")
    assert lines[0].startswith('{"type":"session"')
    assert lines[-1].startswith('{"type":"summary"')
    assert '"seq"' not in raw and '"sid"' not in raw
    assert '"b":1400' in raw
    replay = build_replay_session(raw, f"{SID}.jsonl")
    assert len(replay["path"]) == 151 * 4  # flat [x, y, t, ts, ...]
    assert len(replay["recharges"]) == 1
    assert replay["summary"]["batteryEnd"] == 61

    # Polling again is idempotent.
    await recorder.async_poll()
    assert (await recorder.async_read_session(f"{SID}.jsonl")) == raw


async def test_summary_only_session(hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer) -> None:
    """A clean that ran while HA was away still shows up with its summary."""
    buffer.previous = {"id": SID, "header": HEADER, "summary": SUMMARY}
    await recorder.async_poll()
    sessions = recorder.list_sessions()
    assert len(sessions) == 1
    assert sessions[0]["recording"] is False
    assert sessions[0]["summary"]["duration"] == 600


async def test_deleted_session_is_not_reimported(
    hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer
) -> None:
    """Deleting a finished session sticks even though `previous` still has it."""
    buffer.previous = {"id": SID, "header": HEADER, "summary": SUMMARY}
    await recorder.async_poll()
    await recorder.async_delete_session(f"{SID}.jsonl")
    await recorder.async_poll()
    assert recorder.list_sessions() == []


async def test_cannot_delete_recording(
    hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer
) -> None:
    buffer.current = {"id": SID, "active": True, "header": HEADER}
    buffer.add(sid=SID, x=0.1, y=0.1, t=0.0, ts=1.0)
    await recorder.async_poll()
    with pytest.raises(OpenNeatoApiError):
        await recorder.async_delete_session(f"{SID}.jsonl")


async def test_invalid_names_rejected(hass: HomeAssistant, recorder: EsphomeSessionRecorder) -> None:
    for name in ("../secrets.yaml", "abc.jsonl", ""):
        with pytest.raises(OpenNeatoApiError):
            await recorder.async_read_session(name)


async def test_device_reboot_resets_cursor(
    hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer
) -> None:
    """A new boot id restarts reading from seq 0."""
    buffer.current = {"id": SID, "active": True, "header": HEADER}
    for i in range(10):
        buffer.add(sid=SID, x=0.1 * (i + 1), y=0.0, t=0.0, ts=float(i))
    await recorder.async_poll()

    sid2 = SID + 5000
    buffer.boot = 2
    buffer.entries = []
    buffer.seq = 0
    buffer.current = {"id": sid2, "active": True, "header": {**HEADER, "time": sid2}}
    for i in range(3):
        buffer.add(sid=sid2, x=0.2 * (i + 1), y=0.0, t=0.0, ts=float(i))
    await recorder.async_poll()

    assert buffer.requests[-1] == 0
    names = {s["name"] for s in recorder.list_sessions()}
    assert names == {f"{SID}.jsonl", f"{sid2}.jsonl"}
    raw = await recorder.async_read_session(f"{sid2}.jsonl")
    assert len(build_replay_session(raw)["path"]) == 3 * 4


async def test_zero_sid_gets_ha_time(hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer) -> None:
    """A session started before the device clock was set is named from HA time."""
    buffer.current = {"id": 0, "active": True, "header": {**HEADER, "time": 0}}
    buffer.add(sid=0, x=0.1, y=0.1, t=0.0, ts=1.0)
    await recorder.async_poll()
    (session,) = recorder.list_sessions()
    assert session["name"] != "0.jsonl"
    assert session["session"]["time"] > 1_700_000_000

    buffer.current = None
    buffer.previous = {"id": 0, "header": {**HEADER, "time": 0}, "summary": SUMMARY}
    await recorder.async_poll()
    (session,) = recorder.list_sessions()
    assert session["recording"] is False


async def test_state_survives_reload(
    hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer, tmp_path: Path
) -> None:
    """Session metadata and the cursor are persisted."""
    buffer.current = {"id": SID, "active": True, "header": HEADER}
    buffer.add(sid=SID, x=0.1, y=0.1, t=0.0, ts=1.0)
    await recorder.async_poll()
    await recorder._store.async_save(recorder._data_to_save())

    again = EsphomeSessionRecorder(hass, "entry1", recorder.client, base_dir=str(tmp_path))
    await again.async_load()
    assert again.list_sessions() == recorder.list_sessions()
    assert again._since == 1


async def test_short_session_is_discarded(
    hass: HomeAssistant, recorder: EsphomeSessionRecorder, buffer: PoseBuffer, tmp_path: Path
) -> None:
    """A clean the device dropped (< 3 poses) is dropped here too."""
    buffer.current = {"id": SID, "active": True, "header": HEADER}
    buffer.add(sid=SID, x=0.1, y=0.1, t=0.0, ts=1.0)
    await recorder.async_poll()
    assert len(recorder.list_sessions()) == 1

    buffer.current = None
    await recorder.async_poll()
    assert recorder.list_sessions() == []
    assert not (tmp_path / f"{SID}.jsonl").exists()

    # Its leftover entries in the buffer do not resurrect it.
    buffer.add(sid=SID, x=0.2, y=0.1, t=0.0, ts=3.0)
    await recorder.async_poll()
    assert recorder.list_sessions() == []
