"""Tests for the robot event state machine and entity."""

from __future__ import annotations

import pytest

from custom_components.openneato.event import RobotEventTracker, done_message

IDLE = "UIMGR_STATE_IDLE"
RUNNING = "UIMGR_STATE_HOUSECLEANINGRUNNING"
PAUSED = "UIMGR_STATE_HOUSECLEANINGPAUSED"
SUSPENDED = "UIMGR_STATE_HOUSECLEANINGSUSPENDED"
DOCKING = "UIMGR_STATE_DOCKING"
NO_ERROR = {"hasError": False, "errorCode": 200}


def run(steps: list[tuple[str, str]], error=None) -> list[str]:
    tracker = RobotEventTracker()
    out: list[str] = []
    for ui, robot_state in steps:
        out.extend(t for t, _ in tracker.update(ui, robot_state, error))
    return out


@pytest.mark.parametrize(
    ("steps", "expected"),
    [
        ([(IDLE, ""), (RUNNING, ""), (IDLE, "")], ["cleaning_started", "cleaning_done"]),
        ([(IDLE, ""), (RUNNING, ""), (PAUSED, ""), (RUNNING, "")], ["cleaning_started"]),
        (
            [(IDLE, ""), (RUNNING, ""), (DOCKING, ""), (IDLE, "")],
            ["cleaning_started", "cleaning_done", "returned_to_base"],
        ),
        (
            # Mid-clean recharge: dock, charge (suspended), resume, finish.
            [
                (IDLE, ""),
                (RUNNING, ""),
                (DOCKING, "ST_M1_Charging_Cleaning"),
                (SUSPENDED, "ST_M1_Charging_Cleaning"),
                (RUNNING, ""),
                (IDLE, ""),
            ],
            ["cleaning_started", "docking_recharge", "cleaning_done"],
        ),
        (
            # Clean abandoned while suspended on the dock.
            [(IDLE, ""), (RUNNING, ""), (DOCKING, "ST_M1_Charging_Cleaning"), (SUSPENDED, ""), (IDLE, "")],
            ["cleaning_started", "docking_recharge", "cleaning_done"],
        ),
        ([(IDLE, ""), (DOCKING, ""), (IDLE, "")], ["returned_to_base"]),
        ([(RUNNING, ""), (IDLE, "")], ["cleaning_done"]),
    ],
)
def test_state_transitions(steps, expected) -> None:
    """Transitions follow the legacy NotificationManager rules."""
    assert run(steps) == expected


def test_errors_and_alerts() -> None:
    """New errors/alerts fire once; the condition present at start does not."""
    tracker = RobotEventTracker()
    existing = {"hasError": True, "kind": "error", "errorCode": 243, "displayMessage": "Stuck"}
    assert tracker.update(IDLE, "", existing) == []
    assert tracker.update(IDLE, "", existing) == []
    assert tracker.update(IDLE, "", NO_ERROR) == []
    alert = {"hasError": True, "kind": "warning", "errorCode": 222, "displayMessage": "Empty bin"}
    assert tracker.update(IDLE, "", alert) == [("alert", {"message": "Empty bin", "code": 222})]
    error = {"hasError": True, "kind": "error", "errorCode": 250, "displayMessage": "Brush stuck"}
    assert tracker.update(IDLE, "", error) == [("error", {"message": "Brush stuck", "code": 250})]


def test_done_message() -> None:
    """The done message carries the session stats like the ntfy one did."""
    message, attrs = done_message(
        {"duration": 3600, "areaCovered": 61.5, "distanceTraveled": 412.5, "batteryStart": 100, "batteryEnd": 41}
    )
    assert message == "Cleaning done\n60min | 61.5m² | 412m | 100% → 41%"
    assert attrs["area"] == 61.5
    assert done_message(None) == ("Cleaning done", {})
