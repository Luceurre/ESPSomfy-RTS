# DOOYA Awning Integration

Integration of DOOYA 433 MHz motorized awnings ("store banne", model DC90) into
ESPSomfy RTS. The awning is exposed as a Home Assistant **cover** entity with
open/close/stop commands and travel-time-based position estimation.

## Protocol

The DOOYA protocol is fully reverse-engineered and publicly documented (rtl_433,
Flipper Zero firmware, ESPHome `remote_base`). Our own captures from the DC90
remote are archived in `../dooya_captures/` with the decoder script
`../dooya_decode.py`.

- **Carrier**: 433.92 MHz, ASK/OOK (same as the Create Ikohs fans)
- **Frame**: 40 bits, static code (no rolling counter)
  - 24-bit remote ID (e.g. `62 13 0C`)
  - 8-bit channel (our remote: `0x61`)
  - 4-bit button (1 = up, 3 = down, 5 = stop)
  - 4-bit check — fixed per-button mapping; this remote generation uses
    14/12/5, not the button-echo variant seen on other Dooya remotes
- **Timing** (captured, TE = 335 µs):
  - Sync: 14 TE high (~4.7 ms) + 5 TE low (~1.7 ms)
  - Bit 1: 2 TE on / 1 TE off; bit 0: 1 TE on / 2 TE off
  - Frames repeated back-to-back per key press (we send 3)
- **Captured command bytes** (ID `62130C`, channel `61`):

  | Button | Frame               | Last byte |
  |--------|---------------------|-----------|
  | UP     | `62 13 0C 61 1E`    | `0x1E`    |
  | STOP   | `62 13 0C 61 55`    | `0x55`    |
  | DOWN   | `62 13 0C 61 3C`    | `0x3C`    |

## Software architecture

Mirrors the Create Ikohs fan integration (`Fan.h`):

- **`Dooya.h`** — self-contained `DooyaController`:
  - `dooya_device_t` device model persisted to LittleFS `/awnings.json`
  - `sendDooyaFrame()` bit-bangs the 40-bit OOK frame on the transceiver TX
    pin, retuning the CC1101 to 433.92 around the transmission exactly like
    `FanController::sendFanFrame()` (and retuning back to the Somfy frequency
    afterwards)
  - Position estimation: commands open/close travel with a per-device travel
    time; `loop()` advances the estimated position and auto-sends STOP when the
    target is reached (also powers HA `set_position`)
- **`SomfyController.ino`** — global `dooyaCtrl`, `begin()` at startup, `loop()`
  in the main loop
- **`MQTT.cpp`** — HA MQTT discovery (cover, `device_class: awning`),
  `awnings/+/+/set` subscriptions, command dispatch (OPEN/CLOSE/STOP and
  0-100 position payloads)
- **`Web.cpp`/`Web.h`** — REST endpoints (see below)
- **`data/index.html` + `data/index.js`** — "Awnings" settings subtab (editor
  with name, remote ID hex, channel hex, travel time, room) and home-screen
  card with ▲/■/▼ buttons and live position

## Adding an awning

1. Capture UP/STOP/DOWN from the remote with a Flipper Zero (native `Dooya`
   protocol support). The `Key:` line is `00 00 00 <ID[3]> <channel> <button+check>`.
2. In the web UI: **Settings → Awnings → Add Awning**; enter the name, the
   6-hex-digit remote ID (e.g. `62130C`), the 2-hex-digit channel (e.g. `61`),
   and the full-travel time in seconds.
3. The cover entity appears in Home Assistant via MQTT discovery after the
   device saves (or on the next MQTT reconnect).

## API endpoints

| Endpoint           | Method | Body / args                                  | Description                    |
|--------------------|--------|----------------------------------------------|--------------------------------|
| `/awnings`         | GET    | —                                            | List all awnings               |
| `/awning`          | GET    | `?awningId=n`                                | Fetch one awning               |
| `/awning`          | PUT    | `{awningId, ...fields}`                      | Update an awning               |
| `/awningCommand`   | PUT    | `{awningId, command: up\|down\|stop}`        | Send a command                 |
| `/awningCommand`   | PUT    | `{awningId, command: "position", target: n}` | Move to position 0-100         |
| `/addAwning`       | PUT    | `{awningId?, name, remoteId, channel, travelTime, roomId}` | Create |
| `/saveAwning`      | PUT    | `{awningId, ...fields}`                      | Save edits                     |
| `/deleteAwning`    | PUT    | `{awningId}`                                 | Delete                         |

`remoteId` is the 24-bit remote ID as a decimal number in JSON (the UI converts
from hex).

## MQTT topics

Under the configured root topic:

- `awnings/<id>/cover_command/set` — `OPEN` / `CLOSE` / `STOP`
- `awnings/<id>/position_command/set` — 0-100
- `awnings/<id>/cover_state` — `open` / `opening` / `closed` / `closing` / `stopped`
- `awnings/<id>/position_state` — 0-100
- Discovery: `<discoTopic>/cover/<id>/config`

## Known limitations

- Position is estimated from the configured travel time; there is no RF
  feedback from the motor. If the awning is moved by the original remote the
  estimate drifts. Stop+reopen recalibrates only insofar as travel continues
  from the last estimate.
- The check nibble is per-remote-generation. The captured mapping (14/12/5) is
  hard-coded in `dooya_commands`; a remote using the button-echo variant
  (1/1, 3/3, 5/5) would need its own mapping.
- Single channel byte per awning device; multi-channel remotes need one
  awning entry per channel.
