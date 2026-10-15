"""Cleaning history for the ESPHome backend, recorded on the Home Assistant side.

The ESPHome firmware has no filesystem: it keeps the poses of the current
clean in a RAM ring buffer and serves them through its `get_pose_batch`
action (docs/esphome-contract.md). `EsphomeSessionRecorder` drains that buffer
while the robot is out cleaning and writes each clean to
`<config>/openneato/<entry_id>/<sid>.jsonl` in the same JSONL format the HTTP
firmware stores, so the replay card, the last-clean sensors and the LIDAR
mapper consume it unchanged.
"""

from __future__ import annotations

from collections.abc import Callable
from datetime import timedelta
import json
import logging
import os
import re
import shutil
import time
from typing import TYPE_CHECKING, Any

from homeassistant.core import CALLBACK_TYPE, Event, EventStateChangedData, HomeAssistant, callback
from homeassistant.helpers.event import async_track_state_change_event, async_track_time_interval
from homeassistant.helpers.storage import Store

from .api import OpenNeatoApiError, OpenNeatoConnectionError
from .const import ACTIVE_UISTATE_SUBSTRINGS, DOMAIN, SESSION_NAME_PATTERN

if TYPE_CHECKING:
    from .esphome_backend import OpenNeatoEsphomeClient

_LOGGER = logging.getLogger(__name__)

STORAGE_VERSION = 1
POLL_INTERVAL = timedelta(seconds=5)
# Keep draining for a while after the robot goes idle so the device's final
# poses and the session summary (`previous`) are picked up.
ACTIVE_GRACE_S = 120.0
# While idle, still look once in a while: a clean may have run while Home
# Assistant was down, and its summary is still in `previous`.
IDLE_POLL_S = 600.0
# A session the device no longer reports is closed after this long without a
# summary (robot or bridge rebooted mid-clean).
ORPHAN_TIMEOUT_S = 300.0
# Safety bound on the batch loop; each batch carries at most 100 entries.
MAX_BATCHES_PER_POLL = 20
MAX_SESSIONS = 50
# The device discards sessions that end with fewer poses than this
# (docs/esphome-contract.md); so does the recorder.
MIN_POSES = 3

_NAME_RE = re.compile(SESSION_NAME_PATTERN)
_POSE_KEYS = ("x", "y", "t", "ts", "b")


def _line(obj: dict[str, Any]) -> str:
    return json.dumps(obj, separators=(",", ":")) + "\n"


class EsphomeSessionRecorder:
    """Pull poses from an OpenNeato ESPHome device into JSONL session files."""

    def __init__(
        self,
        hass: HomeAssistant,
        entry_id: str,
        client: OpenNeatoEsphomeClient,
        *,
        base_dir: str | None = None,
    ) -> None:
        """Initialize the recorder for one config entry."""
        self.hass = hass
        self.entry_id = entry_id
        self.client = client
        self.directory = base_dir or hass.config.path(DOMAIN, entry_id)
        self._store: Store[dict[str, Any]] = Store(
            hass, STORAGE_VERSION, f"{DOMAIN}.esphome_history.{entry_id}"
        )
        self._boot: int | None = None
        self._since = 0
        # name -> {"name","size","recording","session","summary","sid","seen"}
        self._sessions: dict[str, dict[str, Any]] = {}
        # Device session ids already closed, so `previous` is not re-imported
        # after the user deletes it.
        self._closed_sids: list[int] = []
        # Name substituted for a session the device stamped with sid 0
        # (its clock was not set yet).
        self._zero_name: str | None = None
        self._last_active = 0.0
        self._last_poll = 0.0
        self._polling = False
        self._unsubs: list[CALLBACK_TYPE] = []
        self._listeners: list[Callable[[], None]] = []
        self._to_remove: list[str] = []

    # ── Lifecycle ────────────────────────────────────────────────────

    async def async_load(self) -> None:
        """Restore the bookkeeping saved by a previous run."""
        data = await self._store.async_load() or {}
        self._boot = data.get("boot")
        self._since = int(data.get("since", 0))
        self._closed_sids = [int(s) for s in data.get("closed_sids", [])]
        self._zero_name = data.get("zero_name")
        for meta in data.get("sessions", []):
            if isinstance(meta, dict) and _NAME_RE.match(str(meta.get("name", ""))):
                self._sessions[meta["name"]] = meta

    @callback
    def async_start(self) -> None:
        """Start following the device."""
        self._unsubs.append(
            async_track_time_interval(self.hass, self._async_tick, POLL_INTERVAL)
        )
        ui_state = self.client.entity_id("text_sensor", "ui_state")
        if ui_state is not None:
            self._unsubs.append(
                async_track_state_change_event(self.hass, ui_state, self._async_ui_state_changed)
            )
        self.hass.async_create_background_task(
            self.async_poll(), f"{DOMAIN} esphome history catch-up"
        )

    @callback
    def async_stop(self) -> None:
        """Stop following the device."""
        while self._unsubs:
            self._unsubs.pop()()

    @callback
    def async_add_listener(self, listener: Callable[[], None]) -> CALLBACK_TYPE:
        """Call `listener` when a session starts, ends or is deleted."""
        self._listeners.append(listener)

        @callback
        def remove() -> None:
            self._listeners.remove(listener)

        return remove

    # ── Public API used by OpenNeatoEsphomeClient ────────────────────

    def list_sessions(self) -> list[dict[str, Any]]:
        """Sessions, newest first, in the /api/history item shape."""
        items = sorted(self._sessions.values(), key=lambda m: m.get("sid", 0), reverse=True)
        return [
            {
                "name": meta["name"],
                "size": meta.get("size", 0),
                "compressed": False,
                "recording": bool(meta.get("recording")),
                "session": meta.get("session"),
                "summary": meta.get("summary"),
            }
            for meta in items
        ]

    async def async_read_session(self, name: str) -> str:
        """Raw JSONL of one session."""
        path = self._path(name)
        if name not in self._sessions:
            raise OpenNeatoApiError(f"No session {name}", status=404)
        try:
            return await self.hass.async_add_executor_job(_read_text, path)
        except FileNotFoundError as err:
            raise OpenNeatoApiError(f"No session {name}", status=404) from err

    async def async_delete_session(self, name: str) -> None:
        """Delete one session file and its metadata."""
        path = self._path(name)
        meta = self._sessions.get(name)
        if meta is None:
            raise OpenNeatoApiError(f"No session {name}", status=404)
        if meta.get("recording"):
            raise OpenNeatoApiError("That session is still being recorded", status=409)
        await self.hass.async_add_executor_job(_remove, path)
        self._sessions.pop(name, None)
        self._save()
        self._notify()

    async def async_remove_all(self) -> None:
        """Delete every recorded file (config entry removal)."""
        self.async_stop()
        await self.hass.async_add_executor_job(shutil.rmtree, self.directory, True)
        await self._store.async_remove()

    # ── Polling ──────────────────────────────────────────────────────

    def _is_active(self) -> bool:
        ui_state = self.client.text("ui_state")
        return any(s in ui_state for s in ACTIVE_UISTATE_SUBSTRINGS)

    def _has_open_session(self) -> bool:
        return any(meta.get("recording") for meta in self._sessions.values())

    @callback
    def _async_ui_state_changed(self, event: Event[EventStateChangedData]) -> None:
        self._last_active = time.monotonic()
        self.hass.async_create_background_task(self.async_poll(), f"{DOMAIN} esphome history")

    async def _async_tick(self, _now: Any = None) -> None:
        now = time.monotonic()
        if self._is_active():
            self._last_active = now
        due = (
            self._is_active()
            or self._has_open_session()
            or now - self._last_active < ACTIVE_GRACE_S
            or now - self._last_poll > IDLE_POLL_S
        )
        if due:
            await self.async_poll()

    async def async_poll(self) -> None:
        """Drain the device's pose buffer into the session files."""
        if self._polling:
            return
        self._polling = True
        self._last_poll = time.monotonic()
        try:
            await self._async_drain()
        except (OpenNeatoApiError, OpenNeatoConnectionError) as err:
            _LOGGER.debug("get_pose_batch failed: %s", err)
        finally:
            self._polling = False

    async def _async_drain(self) -> None:
        changed = False
        pending: dict[str, list[str]] = {}
        reply: dict[str, Any] = {}
        for _ in range(MAX_BATCHES_PER_POLL):
            reply = await self.client.call_action("get_pose_batch", {"since_seq": self._since})
            boot = reply.get("boot")
            if boot != self._boot:
                restarted = self._boot is not None
                self._boot = boot
                if self._since != 0:
                    # Seqs restarted with the device: anything above our old
                    # cursor is meaningless, start over from the beginning.
                    if restarted:
                        _LOGGER.debug("OpenNeato ESPHome device rebooted; restarting pose cursor")
                    self._since = 0
                    continue
            first_seq = int(reply.get("first_seq", 0) or 0)
            if self._since and first_seq > self._since + 1:
                _LOGGER.debug(
                    "Pose buffer wrapped: lost entries %d-%d", self._since + 1, first_seq - 1
                )
            for entry in reply.get("entries") or ():
                if not isinstance(entry, dict):
                    continue
                seq = int(entry.get("seq", 0))
                if seq <= self._since:
                    continue
                self._since = seq
                changed |= self._ingest(entry, reply, pending)
            if not reply.get("more"):
                break

        changed |= self._reconcile(reply, pending)
        while self._to_remove:
            await self.hass.async_add_executor_job(_remove, self._path(self._to_remove.pop()))
        if pending:
            await self.hass.async_add_executor_job(self._append, pending)
            for name in pending:
                meta = self._sessions.get(name)
                if meta is not None:
                    meta["size"] = await self.hass.async_add_executor_job(
                        _size, self._path(name)
                    )
        if changed:
            await self._async_prune()
        self._save()
        if changed:
            self._notify()

    def _name_for(self, sid: int) -> str:
        if sid == 0:
            if self._zero_name is None:
                self._zero_name = f"{int(time.time())}.jsonl"
            return self._zero_name
        return f"{sid}.jsonl"

    def _header_for(self, sid: int, reply: dict[str, Any]) -> dict[str, Any]:
        for key in ("current", "previous"):
            info = reply.get(key)
            if isinstance(info, dict) and info.get("id") == sid and isinstance(info.get("header"), dict):
                header = dict(info["header"])
                break
        else:
            header = {"type": "session", "mode": "house", "time": sid}
        header["type"] = "session"
        if sid == 0:
            header["time"] = int(self._name_for(0).split(".", 1)[0])
        return header

    def _open(self, sid: int, reply: dict[str, Any], pending: dict[str, list[str]]) -> tuple[str, bool]:
        """Session file name for `sid`, creating it (with its header) if new."""
        name = self._name_for(sid)
        if name in self._sessions:
            return name, False
        header = self._header_for(sid, reply)
        self._sessions[name] = {
            "name": name,
            "sid": int(header.get("time", sid) or sid),
            "raw_sid": sid,
            "size": 0,
            "recording": True,
            "session": header,
            "summary": None,
            "seen": time.time(),
            "boot": self._boot,
            "poses": 0,
        }
        pending.setdefault(name, []).append(_line(header))
        return name, True

    def _ingest(self, entry: dict[str, Any], reply: dict[str, Any], pending: dict[str, list[str]]) -> bool:
        sid = int(entry.get("sid", 0) or 0)
        if sid and sid in self._closed_sids:
            return False
        name, created = self._open(sid, reply, pending)
        meta = self._sessions[name]
        if not meta.get("recording"):
            return created
        meta["seen"] = time.time()
        if entry.get("type") == "recharge":
            obj = {"type": "recharge", "x": entry.get("x", 0), "y": entry.get("y", 0)}
        elif "x" in entry and "y" in entry:
            obj = {k: entry[k] for k in _POSE_KEYS if k in entry}
            meta["poses"] = meta.get("poses", 0) + 1
        else:
            return created
        pending.setdefault(name, []).append(_line(obj))
        return created

    def _close(self, name: str, summary: dict[str, Any] | None, pending: dict[str, list[str]]) -> None:
        meta = self._sessions[name]
        meta["recording"] = False
        if summary:
            summary = {**summary, "type": "summary"}
            meta["summary"] = summary
            pending.setdefault(name, []).append(_line(summary))
        raw_sid = int(meta.get("raw_sid", meta.get("sid", 0)) or 0)
        if raw_sid:
            self._closed_sids = [*self._closed_sids[-(MAX_SESSIONS * 2) :], raw_sid]
        if name == self._zero_name:
            self._zero_name = None

    def _discard(self, name: str, pending: dict[str, list[str]]) -> None:
        meta = self._sessions.pop(name)
        pending.pop(name, None)
        self._to_remove.append(name)
        raw_sid = int(meta.get("raw_sid", 0) or 0)
        if raw_sid:
            self._closed_sids = [*self._closed_sids[-(MAX_SESSIONS * 2) :], raw_sid]
        if name == self._zero_name:
            self._zero_name = None

    def _reconcile(self, reply: dict[str, Any], pending: dict[str, list[str]]) -> bool:
        """Open/close sessions from the device's `current` / `previous`."""
        if not reply:
            return False
        changed = False
        current = reply.get("current") if isinstance(reply.get("current"), dict) else None
        previous = reply.get("previous") if isinstance(reply.get("previous"), dict) else None

        if current is not None:
            sid = int(current.get("id", 0) or 0)
            if not (sid and sid in self._closed_sids):
                _name, created = self._open(sid, reply, pending)
                changed |= created

        if previous is not None and isinstance(previous.get("summary"), dict):
            sid = int(previous.get("id", 0) or 0)
            current_sid = int(current.get("id", -1)) if current is not None else None
            if sid != current_sid and not (sid and sid in self._closed_sids):
                if sid == 0 and self._zero_name is None:
                    # A clock-less session we never saw start; nothing to attach it to.
                    pass
                else:
                    name, created = self._open(sid, reply, pending)
                    self._close(name, previous["summary"], pending)
                    changed = True

        # Recording sessions the device no longer reports (it rebooted, or the
        # summary was overwritten by a newer clean before we saw it).
        current_name = (
            self._name_for(int(current.get("id", 0) or 0)) if current is not None else None
        )
        now = time.time()
        for name, meta in list(self._sessions.items()):
            if not meta.get("recording") or name == current_name:
                continue
            if meta.get("boot") == self._boot and meta.get("poses", 0) < MIN_POSES:
                # Ended too short: the device discarded it, so do we.
                self._discard(name, pending)
                changed = True
            elif now - float(meta.get("seen", now)) > ORPHAN_TIMEOUT_S:
                self._close(name, None, pending)
                changed = True
        return changed

    async def _async_prune(self) -> None:
        done = sorted(
            (m for m in self._sessions.values() if not m.get("recording")),
            key=lambda m: m.get("sid", 0),
        )
        excess = len(self._sessions) - MAX_SESSIONS
        for meta in done[: max(0, excess)]:
            await self.hass.async_add_executor_job(_remove, self._path(meta["name"]))
            self._sessions.pop(meta["name"], None)

    # ── Files / storage ──────────────────────────────────────────────

    def _path(self, name: str) -> str:
        if not _NAME_RE.match(name or ""):
            raise OpenNeatoApiError(f"Invalid session name {name!r}", status=400)
        return os.path.join(self.directory, name)

    def _append(self, pending: dict[str, list[str]]) -> None:
        os.makedirs(self.directory, exist_ok=True)
        for name, lines in pending.items():
            with open(self._path(name), "a", encoding="utf-8") as handle:
                handle.writelines(lines)

    @callback
    def _save(self) -> None:
        self._store.async_delay_save(self._data_to_save, 10)

    @callback
    def _data_to_save(self) -> dict[str, Any]:
        return {
            "boot": self._boot,
            "since": self._since,
            "closed_sids": self._closed_sids,
            "zero_name": self._zero_name,
            "sessions": list(self._sessions.values()),
        }

    @callback
    def _notify(self) -> None:
        for listener in list(self._listeners):
            listener()


def _read_text(path: str) -> str:
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def _remove(path: str) -> None:
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def _size(path: str) -> int:
    try:
        return os.path.getsize(path)
    except OSError:
        return 0
