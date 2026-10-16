# OpenNeato — ESPHome firmware

An [ESPHome](https://esphome.io) firmware for the OpenNeato bridge: an ESP32 wired to a Neato
Botvac (D3–D7) serial port exposes the robot to Home Assistant as a native ESPHome device — no
cloud, no app. It is a port of the Arduino firmware in [`../firmware`](../firmware) (same serial
protocol handling, scheduler, cleaning history and manual drive), packaged as an ESPHome external
component, `openneato`.

The [OpenNeato Home Assistant integration](../custom_components/openneato) can use this device as
its backend (vacuum entity, replay card, LIDAR map, notifications). The entity names, actions and
JSON shapes it relies on are a public contract: **[../docs/esphome-contract.md](../docs/esphome-contract.md)**.

## Layout

| Path | What |
|---|---|
| `components/openneato/` | External component. `neato_*.{h,cpp}` are pure C++ (parsers, SKey, serial queue/cache, events, scheduler, history, manual drive) and are host-tested; `openneato.{h,cpp}` + `__init__.py` are the ESPHome glue. |
| `openneato.yaml` | Package: project metadata, API actions, OTA, Wi-Fi + fallback AP + captive portal + Improv, time, UART, hub, bridge diagnostics, factory-reset button. |
| `boards/` | Board packages (pins from `firmware/src/config.h`). |
| `examples/` | Device configs (`openneato-c3.yaml` is annotated) and dummy `secrets.yaml`. |
| `tests/` | Host unit tests (`tests/run.sh`) and build-check configs (`tests/configs/`). |

## Wiring and boards

| Board file | Robot TX→ESP RX | ESP TX→Robot RX | Boot / reset button |
|---|---|---|---|
| `boards/esp32-c3-devkitm-1.yaml` | GPIO4 | GPIO3 | GPIO9 |
| `boards/xiao-esp32c3.yaml` | GPIO20 (D7) | GPIO21 (D6) | GPIO9 |
| `boards/esp32-s3.yaml` | GPIO18 | GPIO17 | GPIO0 |
| `boards/esp32dev.yaml` | GPIO16 | GPIO17 | GPIO0 |

The UART runs at 115200 8N1 (3.3 V logic). See the main [README](../README.md) for the robot-side
connector. Pins can be changed with the `tx_pin` / `rx_pin` / `reset_pin` substitutions.

## Install

1. Create a device config next to a `secrets.yaml` (start from `examples/`):

   ```yaml
   substitutions:
     name: openneato
     friendly_name: OpenNeato

   packages:
     openneato:
       url: https://github.com/Leicas/OpenNeato
       ref: main
       files:
         - esphome/boards/esp32-c3-devkitm-1.yaml
         - esphome/openneato.yaml

   external_components:
     - source: github://Leicas/OpenNeato@main
       components: [openneato]

   api:
     encryption:
       key: !secret api_encryption_key

   wifi:
     ssid: !secret wifi_ssid
     password: !secret wifi_password
   ```

2. Flash over USB the first time: `esphome run openneato.yaml` (or the ESPHome dashboard / web
   installer). Later updates go over the air.
3. Without Wi-Fi credentials the device opens the `OpenNeato Fallback` access point with a captive
   portal; Improv over USB serial also works.
4. Home Assistant discovers the device (ESPHome integration). Adopt it, then add the OpenNeato
   integration and pick the ESPHome device.

Development against this checkout: `cd esphome/examples && esphome run openneato-c3.yaml` (uses the
local component and packages).

Holding the boot button for 5 s factory-resets the bridge (Wi-Fi credentials and restored
settings). The `Restart bridge` / `Factory reset bridge` buttons do the same from Home Assistant.

## Dashboard

[`dashboard.yaml`](dashboard.yaml) is a Home Assistant dashboard with every command and entity (map card,
cleaning controls, schedule, settings, manual drive, diagnostics). Paste it into a new dashboard's raw
configuration editor; it assumes the entity prefix `neato` (replace every `.neato` with yours, e.g.
`.bureau_neato` when Home Assistant adds the area).

## Hub options

```yaml
openneato:
  id: neato                 # the package's actions call id(neato)
  uart_id: robot_uart
  time_id: ha_time          # optional; without it the schedule, clock sync and session timestamps are off
  pose_buffer_size: 256     # RAM ring buffer for get_pose_batch (256-4096, ~28 B each)
  robot_clock: utc          # utc | local — which time `SetTime All` writes to the robot RTC
  idle_poll_interval: 10s   # GetState/GetErr cadence while idle (2 s while active)
  # Every contract entity is created automatically; each accepts an optional
  # override block, e.g.:
  brush_rpm:
    disabled_by_default: false
```

Keep the default entity names: the integration finds entities by their object id.

## Entities and actions

All entities (`text_sensor`, `sensor`, `binary_sensor`, `switch`, `datetime`, `select`, `number`,
`button`, `event`) and API actions (`clean`, `play_sound`, `set_user_setting`, `send_command`,
`get_lidar`, `get_pose_batch`, `manual_move`) are listed in
[../docs/esphome-contract.md](../docs/esphome-contract.md). Home Assistant exposes the actions as
`esphome.<node_name>_<action>`; all return JSON (`supports_response: optional`) and fail with
`success: false` + an error message.

`send_command` and `get_lidar` wait for the robot: the action queues the serial command, then a
`wait_until` (5 s) on the hub's request table, then `api.respond` and cleanup. The pending API call
stays registered while waiting (ESPHome appends its unregister step after the whole action chain),
so the response is delivered asynchronously without blocking the main loop.

## Behaviour notes

- **Polling budget** (one command in flight, 50 ms gap, 3 s timeout, framed on `0x1A`):
  GetState+GetErr every 2 s while active (10 s idle), GetRobotPos Smooth + GetMotors every 2 s while
  recording a session, GetCharger 15 s active / 30 s idle, GetAnalogSensors / GetDigitalSensors /
  GetMotors 60 s, GetUserSettings 5 min, GetWarranty 30 min, GetVersion 1 h. Manual mode adds the
  500 ms bumper / stall polls.
- **Robot connected** turns on once `GetVersion` answered (the SetEvent key is derived from the
  serial number) and off when a command times out.
- **Clock sync**: `SetTime All YYYY-MM-DD--hh:mm:ss` once the time is valid, every 4 h and on the
  `Sync robot clock` button.
- **Schedule**: 7 days × 2 slots run on the bridge (local time of the time source); a slot fires 0–5 min
  after its time when the robot is idle. `Skip next clean` is consumed by the next due slot (and
  `Next clean` already shows the one after). `Restart before clean` power-cycles the robot and
  starts the clean once it is idle again (2 min timeout).
- **Cleaning history**: sessions are recorded in RAM only; Home Assistant pulls them with
  `get_pose_batch` and writes the JSONL files. Sessions with fewer than 3 poses are discarded
  (`previous` keeps the last kept session).
- **Manual mode**: TestMode + LDS on; bumper contact / wheel lift / stall (wheel load above
  `Stall threshold` for 3 s) block moves in that direction; wheels stop when no `manual_move`
  arrives for 5 s.

## Tests

```bash
esphome/tests/run.sh            # builds with clang++/g++ -std=c++17 -Wall -Wextra (+ASan/UBSan) and runs
esphome/tests/run.sh history    # only tests whose name contains "history"
cd esphome/examples && esphome config openneato-c3.yaml && esphome compile openneato-c3.yaml
cd esphome/tests/configs && esphome compile openneato-c3-arduino.yaml   # Arduino-framework build check
```

## License

MIT, like the rest of OpenNeato. Ported files carry an attribution header.
