# ESPHome firmware ↔ Home Assistant integration contract

The ESPHome firmware (`esphome/components/openneato`) and the `openneato` HA integration's
ESPHome backend (`custom_components/openneato/esphome_backend.py`) talk only through what is
listed here. Treat every name below as a public API: renaming an entity or action breaks the
integration.

Home Assistant derives ESPHome entity unique IDs as `{mac}-{entity_type}-{object_id}`, where
`object_id` is the snake_cased entity name. The integration resolves entities by
`(entity_type, object_id)` on the ESPHome device, so the **default names below must not change**.
Users may still rename entities in Home Assistant (entity_id/friendly name) without breaking
anything.

The firmware requires ESPHome ≥ 2025.11. The ESPHome device must declare `esphome: project: name: openneato.esphome` so the integration
can discover it (HA's ESPHome integration then reports manufacturer `openneato`, model `esphome`).

## Entities

All entities are created by the `openneato:` hub with these default names.

### text_sensor

| Name | object_id | Value |
|---|---|---|
| UI state | `ui_state` | raw `UIMGR_STATE_*` from `GetState` (patched to `UIMGR_STATE_MANUALCLEANING` while manual mode is active) |
| Robot state | `robot_state` | raw `ST_*` from `GetState` (empty on old robot firmware) |
| Activity | `activity` | one of `cleaning`, `paused`, `returning`, `docked`, `idle`, `error`, `manual` |
| Error message | `error_message` | human-readable message (`displayMessage`), empty when no error |
| Error kind | `error_kind` | `error` (codes ≥ 243), `warning` (201–242) or empty |
| Model | `model` | e.g. `BotVacD7Connected` (diagnostic) |
| Serial number | `serial_number` | robot serial (diagnostic) |
| Robot firmware | `robot_firmware` | `Major.Minor.Build` (diagnostic) |
| LDS firmware | `lds_firmware` | diagnostic |
| Main board | `main_board` | diagnostic |

### sensor

| Name | object_id | Unit / class |
|---|---|---|
| Battery | `battery` | `%`, battery |
| Battery voltage | `battery_voltage` | V, voltage |
| Battery current | `battery_current` | mA, current (diagnostic) |
| Battery temperature | `battery_temperature` | °C, temperature (diagnostic) |
| External voltage | `external_voltage` | V, voltage (diagnostic) |
| Battery cycles | `battery_cycles` | — (diagnostic) |
| Total cleaning time | `total_cleaning_time` | s, duration (diagnostic) |
| Error code | `error_code` | — (200 = none) |
| Brush RPM | `brush_rpm` | rpm (diagnostic, disabled by default) |
| Vacuum RPM | `vacuum_rpm` | rpm (diagnostic, disabled by default) |
| Left wheel load | `left_wheel_load` | % (diagnostic, disabled by default) |
| Right wheel load | `right_wheel_load` | % (diagnostic, disabled by default) |
| LDS RPM | `lds_rpm` | rpm (diagnostic, disabled by default) |
| Next clean | `next_clean` | timestamp (unknown when schedule off / nothing enabled) |
| Clean started | `clean_started` | timestamp of current/last session start |
| Clean duration | `clean_duration` | s, duration (live during a clean, final afterwards) |
| Clean area | `clean_area` | m² |
| Clean distance | `clean_distance` | m |
| Clean recharges | `clean_recharges` | — |
| Clean battery used | `clean_battery_used` | % |

Timestamp sensors are ESPHome float32 values, so `next_clean` / `clean_started` are only exact to about ±64 s.

### binary_sensor

| Name | object_id | Class |
|---|---|---|
| Charging | `charging` | battery_charging |
| External power | `external_power` | plug |
| Problem | `problem` | problem (true for errors **and** warnings) |
| Dustbin | `dustbin` | — (true = bin inserted) |
| Wheel lifted | `wheel_lifted` | — |
| Front left bumper | `front_left_bumper` | — (diagnostic) |
| Front right bumper | `front_right_bumper` | — (diagnostic) |
| Side left bumper | `side_left_bumper` | — (diagnostic) |
| Side right bumper | `side_right_bumper` | — (diagnostic) |
| Battery overheating | `battery_overheating` | heat (diagnostic) |
| Battery failure | `battery_failure` | problem (diagnostic) |
| Robot connected | `robot_connected` | connectivity (diagnostic) — true once the robot answered `GetVersion` and the last command did not time out |
| Stall front | `stall_front` | — (manual mode) |
| Stall rear | `stall_rear` | — (manual mode) |

### switch

| Name | object_id | Backing |
|---|---|---|
| Eco mode | `eco_mode` | `SetUserSettings EcoMode` |
| Intense clean | `intense_clean` | `SetUserSettings IntenseClean` |
| Bin full detection | `bin_full_detection` | `BinFullDetect` |
| Wall following | `wall_following` | `WallEnable` |
| Button click sound | `button_click_sound` | `ButtonClick` |
| Melodies | `melodies` | `Melodies` |
| Warning sounds | `warning_sounds` | `Warnings` |
| Stealth LED | `stealth_led` | `StealthLED` |
| Schedule | `schedule` | on-device scheduler master switch (restored) |
| Skip next clean | `skip_next_clean` | skips the next enabled slot (restored) |
| Restart before clean | `restart_before_clean` | power-cycle robot before a scheduled clean (restored) |
| Schedule {Day} {N} | `schedule_{day}_{n}` | slot enabled, day ∈ monday…sunday, n ∈ 1,2 (restored) |
| Manual mode | `manual_mode` | TestMode + LDS on/off |
| Manual brush | `manual_brush` | manual mode only |
| Manual vacuum | `manual_vacuum` | manual mode only |
| Manual side brush | `manual_side_brush` | manual mode only |

### datetime (type: time)

| Name | object_id |
|---|---|
| Schedule {Day} {N} time | `schedule_{day}_{n}_time` (restored) |

### select

| Name | object_id | Options |
|---|---|---|
| Navigation mode | `navigation_mode` | `Normal`, `Gentle`, `Deep`, `Quick` (sent before each house clean, restored) |
| Sound | `sound` | the 21 `PlaySound` names (`Waking up` … `Thank you`) |

### number

| Name | object_id | Unit / range |
|---|---|---|
| Filter change interval | `filter_change_interval` | days, 1–365 (`FilterChange` seconds) |
| Brush change interval | `brush_change_interval` | days, 1–365 (`BrushChange` seconds) |
| Dirt bin reminder | `dirt_bin_reminder` | min, 1–600 (`DirtBin` minutes) |
| Stall threshold | `stall_threshold` | %, 30–80 (restored) |
| Manual brush speed | `manual_brush_speed` | rpm, 500–1600 (restored) |
| Manual vacuum speed | `manual_vacuum_speed` | %, 40–100 (restored) |
| Manual side brush power | `manual_side_brush_power` | mW, 500–1500 (restored) |

### button

| Name | object_id |
|---|---|
| Start house clean | `start_house_clean` |
| Start spot clean | `start_spot_clean` |
| Pause | `pause` |
| Stop | `stop` |
| Return to base | `return_to_base` |
| Locate | `locate` (plays sound 19) |
| Play sound | `play_sound` (plays the `Sound` select) |
| Clear errors | `clear_errors` |
| Restart robot | `restart_robot` |
| Shut down robot | `shut_down_robot` |
| New battery | `new_battery` |
| Sync robot clock | `sync_robot_clock` |
| Start mapping run | `start_mapping_run` (`Clean Explore`) |

### event

| Name | object_id | Event types |
|---|---|---|
| Robot event | `robot_event` | `cleaning_started`, `cleaning_done`, `docking_recharge`, `returned_to_base`, `error`, `alert` |

Semantics follow the legacy `NotificationManager` (`ui` = UI state, `prev` = previous UI state,
idle = `UIMGR_STATE_IDLE` or `UIMGR_STATE_STANDBY`, recharging = robot state contains
`Charging_Cleaning`; `cleaningBeforeDock` is set on `CLEANINGRUNNING` → `DOCKING` and cleared
when leaving `DOCKING` for anything but `CLEANINGSUSPENDED`, and on idle):

- `cleaning_started`: `ui` contains `CLEANINGRUNNING` and `prev` contained neither `CLEANING` nor `DOCKING`.
- `docking_recharge`: `ui` enters `DOCKING` while recharging.
- `cleaning_done`: `ui` is idle and `prev` was `CLEANINGRUNNING`, or `prev` was `DOCKING` with
  `cleaningBeforeDock` and not recharging, or `prev` was `CLEANINGSUSPENDED` with `cleaningBeforeDock`.
- `returned_to_base`: `prev` was `DOCKING`, `ui` is idle, not recharging (fires together with
  `cleaning_done` when a clean ends on the dock).
- `error` / `alert`: a new error (code ≥ 243) / warning (201–242) appears or its code changes. Not
  fired for the condition already present at boot.

The message is in the `Error message` text sensor. The integration's own `event` entity (HTTP
backend) uses the same event types and adds a `message` attribute.

## Actions

Defined in the shipped package (`esphome/openneato.yaml`, `api: actions:`) on top of hub
methods; Home Assistant exposes them as `esphome.<node_name>_<action>` (dashes in the node name
become underscores). All actions return JSON via `api.respond` (`supports_response: optional`).
Failures respond `success: false` with an `error_message`. Actions that need a serial round trip
(`send_command`, `get_lidar`) wait for the robot (≤ 5 s) before responding.

| Action | Variables | Response `data` |
|---|---|---|
| `clean` | `action: string` — `house` (start, or resume when paused), `spot`, `pause`, `stop`, `dock`, `explore` (mapping run: `Clean Explore`, the Smart App exploration run, no cleaning) | `{}` |
| `play_sound` | `sound_id: int` (0–20) | `{}` |
| `set_user_setting` | `key: string`, `value: string` — `SetUserSettings` key/value (`EcoMode`, `ON`) | `{}` |
| `send_command` | `command: string` — raw serial command | `{"response": "<raw text including the command echo, without 0x1A>"}` |
| `get_lidar` | — | `{"rotationSpeed": float, "validPoints": int, "dist": [360 ints mm], "intensity": [360 ints], "error": [360 ints]}` |
| `get_pose_batch` | `since_seq: int` | see below |
| `manual_move` | `left_mm: int`, `right_mm: int`, `speed: int` | `{}` (error `blocked` / `inactive`) |

### `get_pose_batch`

The device records a session while the robot cleans (same rules as the legacy
`CleaningHistory`: starts on house/spot cleaning states, pauses collection while paused or
recharging, ends when idle; drops the origin-reset pose; polls `GetRobotPos Smooth` every 2 s).
Entries go into a RAM ring buffer (≥ 256 entries) with a monotonically increasing `seq`
(never reset during uptime). The buffer is **not** cleared when a session ends.

Request returns at most 100 entries with `seq > since_seq`, oldest first:

```json
{
  "boot": 3735928559,
  "seq": 1234,
  "first_seq": 980,
  "more": false,
  "current": {"id": 1771683615, "active": true,
              "header": {"type": "session", "mode": "house", "time": 1771683615, "battery": 98}},
  "previous": {"id": 1771600000,
               "header": {"type": "session", "mode": "house", "time": 1771600000, "battery": 100},
               "summary": {"type": "summary", "time": 1771603600, "duration": 3600, "mode": "house",
                           "recharges": 0, "snapshots": 1800, "distanceTraveled": 412.5,
                           "maxDistFromOrigin": 9.1, "totalRotation": 52000.0, "areaCovered": 61.5,
                           "errorsDuringClean": 0, "batteryStart": 100, "batteryEnd": 41}},
  "entries": [
    {"seq": 1233, "sid": 1771683615, "x": 1.234, "y": -0.5, "t": 90.0, "ts": 1234.5, "b": 1400},
    {"seq": 1234, "sid": 1771683615, "type": "recharge", "x": 1.3, "y": -0.4}
  ]
}
```

- `boot`: random 32-bit value chosen at boot; a change means seq restarted.
- `seq`: highest seq ever assigned; `first_seq`: oldest seq still in the buffer.
- `more`: true when more entries with `seq > last returned` exist.
- `current`: the session being recorded, or `null` when idle. `active` false while paused/recharging.
- `previous`: the last completed session (header + summary), or `null`.
- `sid`: id of the session the entry belongs to (= header `time`, epoch seconds; if the clock was
  not valid at session start the device uses `0` and the integration substitutes its own time).
- Only house and spot cleans and mapping runs are recorded (not manual driving). A session that
  starts within 5 min of an `explore` request has header `mode: "explore"`. A session that ends with fewer than
  3 poses is discarded: `current` becomes `null` without it becoming `previous`, so the integration
  drops its `sid` (its entries may stay in the buffer).
- Pose `b`: main brush RPM read (`GetMotors`) right after the pose, omitted when that read failed.
  The replay only paints poses with the brush turning as cleaned floor.
- Header/summary/pose/recharge objects use the exact field names of the legacy JSONL format so
  the integration can write them verbatim (pose objects are written without `seq`/`sid`).

## Hub YAML (reference)

```yaml
external_components:
  - source: github://Leicas/OpenNeato@main
    components: [openneato]
    # path: esphome/components   (set when using type: git)

uart:
  id: robot_uart
  tx_pin: ${tx_pin}
  rx_pin: ${rx_pin}
  baud_rate: 115200
  rx_buffer_size: 4096

time:
  - platform: homeassistant
    id: ha_time

openneato:
  id: neato
  uart_id: robot_uart
  time_id: ha_time
```
