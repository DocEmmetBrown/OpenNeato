# OpenNeato ESPHome firmware with Home Assistant

The OpenNeato ESPHome firmware is an alternative to the OpenNeato web-UI firmware for people who run Home
Assistant. The same ESP32 board, wired the same way, talks to the robot over UART, but instead of serving its
own web UI it is an [ESPHome](https://esphome.io) device: Home Assistant's ESPHome integration gets every sensor,
switch, button, schedule slot and the robot event natively, and the OpenNeato integration adds what ESPHome
cannot express — the `vacuum` entity, the cleaning history and replay card, and the LIDAR floor plan.

| | Web-UI firmware (`firmware/`) | ESPHome firmware (`esphome/`) |
|---|---|---|
| Standalone web UI, ntfy, on-device history files | ✅ | — |
| Configured in | its web UI | ESPHome YAML + Home Assistant |
| Entities | created by the OpenNeato integration over HTTP | native ESPHome entities (push, no polling) |
| Vacuum entity, replay card, LIDAR map | OpenNeato integration | OpenNeato integration |
| Notifications | ntfy, or the `Robot event` entity + blueprint | `Robot event` entity + blueprint |
| Updates | OTA from its web UI | ESPHome OTA |

The entity and action names shared by the firmware and the integration are specified in
[esphome-contract.md](esphome-contract.md).

## Requirements

- Home Assistant 2025.10 or newer (ESPHome actions that return data), with the OpenNeato integration installed
  from HACS (see the [README](../README.md#install-via-hacs)).
- ESPHome 2025.11 or newer to build the firmware.
- The bridge hardware and wiring from the [README](../README.md#installation) — nothing changes there.

## 1. Build and flash the firmware

See [esphome/README.md](../esphome/README.md) for the YAML to start from, the supported boards and the first
(USB) flash. In short, a node only needs the shared package, its board and the component:

```yaml
substitutions:
  name: neato
  friendly_name: Neato

packages:
  openneato:
    url: https://github.com/Leicas/OpenNeato
    ref: main
    files: [esphome/boards/esp32-c3-devkitm-1.yaml, esphome/openneato.yaml]

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

Later updates are regular ESPHome OTA updates.

## 2. Add the device to Home Assistant

1. **ESPHome**: Home Assistant discovers the node; add it under **Settings → Devices & services → ESPHome**
   (enter the API encryption key if you set one).
2. **OpenNeato**: the integration discovers the device as well (its mDNS record carries
   `project_name: openneato.esphome`) and offers to set it up. Confirm. You can also add it by hand:
   **Add integration → OpenNeato → OpenNeato ESPHome firmware** and pick the device.

The OpenNeato entry creates one device, the robot (named after the ESPHome node and linked to it as
"connected via"), with the `vacuum` entity. Everything else is on the ESPHome device.

## 3. Cleaning history and the map card

The ESPHome firmware has no filesystem. While the robot cleans it keeps the poses in a RAM ring buffer, and the
integration drains it every 5 seconds into `<config>/openneato/<entry_id>/<session>.jsonl` — the same format the
web-UI firmware stores, so the replay card, the last-clean sensors and the LIDAR floor plan work unchanged. The
50 most recent sessions are kept; deleting one from the card deletes its file.

Cleans with fewer than 3 poses are discarded by the device and by the integration. Manual driving is not
recorded. Home Assistant must be running during a clean to record its path. If it was not, the session still appears with
its summary (duration, area, distance, battery) once Home Assistant reconnects, but without a path.

## 4. Notifications

Both firmwares have a **Robot event** entity firing `cleaning_started`, `cleaning_done`, `docking_recharge`,
`returned_to_base`, `error` and `alert`. The bundled blueprint turns them into notifications through any
`notify` action (mobile app, Telegram, …):

[![Import blueprint](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2FLeicas%2FOpenNeato%2Fblob%2Fmain%2Fblueprints%2Fautomation%2Fopenneato%2Fnotify.yaml)

Pick the robot's `Robot event` entity, the notify action and the event types you want. Error and alert texts come
from the robot (`Error message`); `cleaning_done` includes the duration and area.

## Migrating from the web-UI firmware

1. Flash the ESPHome firmware on the bridge (over USB; the web UI's OTA only accepts its own images).
2. Add the device to the ESPHome integration.
3. Add it to OpenNeato (discovery or **Add integration**). When the robot's serial number matches an existing
   OpenNeato entry, that entry is switched to the ESPHome firmware in place, so the vacuum entity and its
   automations are kept. The old entry's other entities are removed; their equivalents are on the ESPHome
   device (update dashboards and automations that used them).

Cleaning history stored on the old bridge is not carried over. Schedules and settings that lived on the bridge
(the robot's own user settings stay on the robot) must be set again on the ESPHome entities.

## Troubleshooting

- **"The ESPHome device has no action …"** — the node does not run the OpenNeato package, or it is an older
  build. Reflash it with the current `esphome/openneato.yaml`.
- **The OpenNeato entry keeps retrying** — the ESPHome device is offline, or the robot has not answered yet
  (`Robot connected` is off): check the UART wiring and that the robot is switched on.
- **No path in the history** — Home Assistant was not running, or the device restarted, during the clean.
