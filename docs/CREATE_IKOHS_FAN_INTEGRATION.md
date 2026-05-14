# Create Ikohs Ceiling Fan Integration with ESPSomfy RTS

## Overview

This document describes the integration of Create Ikohs ceiling fan RF control into the
ESPSomfy RTS firmware, enabling a single ESP32 + CC1101 device to control both Somfy RTS roller
shutters and Create Ikohs ceiling fans.

## Hardware

- **ESP32-WROVER** module
- **CC1101** sub-GHz transceiver (shared with Somfy RTS)
- **Create Ikohs** ceiling fan with RF remote (433.92MHz)

The CC1101 is shared between Somfy RTS (rolling code protocol) and the fan protocol (static code).
The firmware switches the CC1101 frequency between 433.42MHz (Somfy) and 433.92MHz (fan) as needed.

## Protocol Details

The Create Ikohs fan uses a simple static-code OOK (On-Off Keying) protocol on 433.92MHz.

### Frame Format

```
| Preamble    | 32-bit Code                                       |
|-------------|-----------------------------------------------------|
| 250us HIGH  | N0 | N1 | N2 | N3 | N4 | N5 | N6 | N7         |
| 8ms LOW    |                                                     |
```

Repeated **8 times** per command transmission.

### Bit Encoding (OOK PWM)

| Bit | HIGH time | LOW time |
|-----|-----------|----------|
| 0   | 250us     | 750us    |
| 1   | 750us     | 250us    |

### Code Structure (32-bit)

| Nibble | Bits     | Description                             |
|--------|----------|-----------------------------------------|
| N0     | 31-28    | Address nibble 1                         |
| N1     | 27-24    | Address nibble 2                         |
| N2     | 23-20    | Address nibble 3                         |
| N3     | 19-16    | Address nibble 4                         |
| N4     | 15-12    | Address nibble 5                         |
| N5     | 11-8     | Command (upper 4 bits of 5-bit command)  |
| N6     | 7-4      | Bit 3 = lowest command bit; Bits 2-0 = 3-bit rolling counter |
| N7     | 3-0      | Checksum (XOR-based)                     |

**Address**: 20-bit value across N0-N4 (range: 1 to 1,048,575). Each fan remote has a unique
address. The decimal address is converted: e.g., 542295 = 0x84657 = N0=8, N1=4, N2=6, N3=5, N4=7.

**5-bit Command**: Split across N5 (upper 4 bits) and N6 bit 3 (lowest bit). N5 = `(cmd >> 1)`,
N6[3] = `(cmd & 1)`. The remaining 3 bits of N6 form a rolling counter (wraps 0-7).

**Checksum (N7)**: `(N0 ^ N1 ^ N2 ^ N3 ^ N4 ^ N5 ^ N6 ^ 0x0A) & 0x0F`

### Commands

| Name   | 5-bit Code | N5   | N6[3] | Description                     |
|--------|------------|------|-------|---------------------------------|
| light  | 0x12       | 0x9  | 0     | Toggle light on/off             |
| fan    | 0x0A       | 0x5  | 0     | Toggle fan on/off               |
| color  | 0x1C       | 0xE  | 0     | Cycle light color               |
| speed1 | 0x04       | 0x2  | 0     | Set fan speed to 1              |
| speed2 | 0x10       | 0x8  | 0     | Set fan speed to 2              |
| speed3 | 0x0C       | 0x6  | 0     | Set fan speed to 3              |
| speed4 | 0x06       | 0x3  | 0     | Set fan speed to 4              |
| speed5 | 0x09       | 0x4  | 1     | Set fan speed to 5              |
| speed6 | 0x15       | 0xA  | 1     | Set fan speed to 6              |

## Reverse Engineering Methodology

The protocol was reverse-engineered using:
1. **Flipper Zero** (Momentum firmware) to capture raw sub-GHz signals from the original remote
2. **Universal Radio Hacker (URH)** to analyze signal timing, bit patterns, and identify structure
3. **Python scripts** to decode nibble values, test checksum hypotheses, and verify across multiple captures

Key findings:
- Initial analysis assumed N5 was the full 4-bit command and N6 was unused. However, two button
  pairs mapped to the same N5 value (Speed 5/Mute both N5=0x4, Speed 2/Invert Rotation both N5=0x8).
  Further analysis revealed N6[3] is the 5th command bit, while N6[2:0] is a rolling counter.
- The checksum N7 uses XOR with constant 0x0A
- Each fan remote has a fixed address that doesn't change

## Software Architecture

### Files Modified

| File                    | Description                                        |
|-------------------------|----------------------------------------------------|
| `Fan.h`                 | FanController class, protocol encoding, LittleFS persistence |
| `SomfyController.ino`   | Wire FanController into setup(), shares CC1101 transceiver |
| `Web.h`                 | Fan handler method declarations                      |
| `Web.cpp`               | Fan API endpoints, parseFanCommand helper              |
| `data/index.html`        | Fan settings tab, fan edit form, fan home container |
| `data/index.js`          | Fan card rendering, fan API methods, room filtering    |

### FanController Class (Fan.h)

```cpp
class FanController {
    Transceiver &transceiver;
    fan_device_t fans[MAX_FANS];  // MAX_FANS = 16

    bool begin();                              // Load fans from /fans.json
    bool sendCommand(uint8_t fanId, fan_commands cmd);
    fan_device_t *addFan();
    bool deleteFan(uint8_t fanId);
    fan_device_t *getFanById(uint8_t fanId);
    void toJSONFans(JsonResponse &json);
    bool saveFans();
    bool loadFans();
};
```

Key implementation details:
- `sendFanFrame()` uses direct GPIO bit-banging (not CC1101 packet mode) because the fan protocol
  uses OOK PWM encoding, not the CC1101's built-in packet handler
- Frequency is temporarily switched to 433.92MHz during transmission, then restored to the Somfy frequency
- Fan data persists in `/fans.json` on LittleFS

### API Endpoints

| Method | Endpoint           | Body                                    | Response            |
|--------|--------------------|-----------------------------------------|---------------------|
| GET    | `/fans`             | —                                       | Array of fan objects |
| GET    | `/fan?fanId=X`       | —                                       | Single fan object   |
| PUT    | `/fanCommand`        | `{fanId, command}`                      | Updated fan object |
| PUT    | `/addFan`           | `{name, address, roomId}`               | Created fan object |
| PUT    | `/saveFan`          | `{fanId, name, address, roomId, sortOrder}`| Updated fan object |
| PUT    | `/deleteFan`        | `{fanId}`                               | Status             |
| GET    | `/getNextFanId`     | —                                       | `{fanId}`            |

The `/command` field in `/fanCommand` accepts both string names (`"light"`, `"speed1"`) and numeric
codes (`9`, `2`).

### Web UI

- **Home panel**: Fan control cards appear below shade cards. Each card has a lightbulb icon, fan name,
  and a row of circular buttons: light toggle, fan toggle, speed 1-6.
- **Settings → Somfy → Fans**: Fan management tab with list, add/edit form (name, address, room),
  and test command buttons for pairing verification.
- Fan cards are filtered by the room selector, same as shades.

## Building and Flashing

### Prerequisites

- Arduino CLI with ESP32 core installed
- `esptool` Python package (in the Arduino venv)
- CC1101 library (`ELECHOUSE_CC1101_SRC_DRV`)

### Build Command

```bash
# Create symlink directory (required because .ino name != directory name)
rm -rf /tmp/SomfyController
mkdir -p /tmp/SomfyController
ln -sf /path/to/ESPSomfy-RTS/*.ino \
       /path/to/ESPSomfy-RTS/*.cpp \
       /path/to/ESPSomfy-RTS/*.h \
       /tmp/SomfyController/

# Compile with custom partition table (no OTA — larger app partition needed)
arduino-cli compile \
  --clean \
  --output-dir /tmp/SomfyController/build \
  --fqbn esp32:esp32:esp32wrover:PartitionScheme=no_ota,FlashMode=qio,FlashFreq=80,UploadSpeed=921600 \
  /tmp/SomfyController
```

### Custom Partition Table

The default `no_ota` partition scheme doesn't include a LittleFS partition. A custom partition
table was created to accommodate the larger firmware binary:

```
| Name     | Type  | Subtype | Offset    | Size       |
|----------|-------|---------|-----------|------------|
| nvs      | data  | 2       | 0x9000    | 0x5000     |
| otadata  | data  | 0       | 0xE000    | 0x2000     |
| app0     | app   | 0x10    | 0x10000   | 0x280000   |
| spiffs   | data  | 0x82    | 0x290000  | 0x160000   |
| coredump | data  | 0x03    | 0x3F0000  | 0x10000    |
```

The partition table binary format:
- Magic: `0x50 0xAA` at bytes 0-1
- Type at byte 2, subtype at byte 3
- Offset (uint32 LE) at bytes 4-7
- Size (uint32 LE) at bytes 8-11
- Name (16 bytes, null-terminated) at **byte 12** (not 16)
- Flags (uint32 LE) at bytes 28-31
- Entry size: 32 bytes

### Flashing

```bash
# Flash firmware
~/.arduino15-venv/bin/python -m esptool --port /dev/ttyUSB0 --baud 115200 \
  --chip esp32 write_flash 0x10000 /tmp/SomfyController/build/SomfyController.ino.bin

# Flash custom partition table (only needed once)
~/.arduino15-venv/bin/python -m esptool --port /dev/ttyUSB0 --baud 115200 \
  --chip esp32 write_flash 0x8000 /tmp/custom_partitions_v2.bin

# Upload web files to LittleFS
MKLITTLEFS=~/.arduino15/packages/esp32/tools/mklittlefs/3.0.0-gnu12-dc7f933/mklittlefs
$MKLITTLEFS -c /path/to/ESPSomfy-RTS/data -p 256 -b 4096 -s 1441792 /tmp/littlefs.bin
~/.arduino15-venv/bin/python -m esptool --port /dev/ttyUSB0 --baud 115200 \
  --chip esp32 write_flash 0x290000 /tmp/littlefs.bin
```

**IMPORTANT**: The LittleFS partition size MUST match the partition table exactly. Read the actual
size from the partition table — do NOT calculate it from offset differences. The spiffs partition in our
table is `0x160000 = 1,441,792 bytes`, not the gap to the next partition.

### Adding Fans via API

After flashing, add fans using the web UI or API:

```bash
# Add a fan
curl -X PUT -H "Content-Type: application/json" \
  -d '{"name":"Living Room Fan","address":542295,"roomId":0}' \
  http://192.168.0.46/addFan

# Send a command
curl -X PUT -H "Content-Type: application/json" \
  -d '{"fanId":1,"command":"light"}' \
  http://192.168.0.46/fanCommand
```

## Pairing a Fan

1. Go to **Settings → Somfy → Fans** in the web UI
2. Click **Add Fan**, enter a name and the 20-bit decimal address from the original remote
3. Use the test buttons in the edit form to send commands — the fan will **beep** when it receives
4. If the fan responds correctly, click **Save Fan**

The address can be decoded from captured signals by extracting N0-N4 nibbles and computing:
`address = (N0 << 16) | (N1 << 12) | (N2 << 8) | (N3 << 4) | N4`

## Known Limitations

- No feedback from the fan (stateless protocol — no position/state reporting)
- Fan shares the CC1101 with Somfy RTS; fan commands temporarily switch frequency
- Maximum 16 fans supported (`MAX_FANS` in Fan.h)
- No OTA support — firmware binary too large for default OTA partition, requires USB flash
- Rolling codes are NOT used (static 32-bit code with 3-bit counter), unlike Somfy RTS
- Only 9 of 14 remote buttons exposed in firmware (Cooldown timers, Mute, Invert Rotation decoded but not yet in UI)

## Backup & Recovery

Always backup before flashing:

```bash
# Full 4MB flash backup
esptool --port /dev/ttyUSB0 --baud 115200 --chip esp32 read_flash 0x0 0x400000 full_backup.bin

# LittleFS only (partition size from partition table)
esptool --port /dev/ttyUSB0 --baud 115200 --chip esp32 read_flash 0x290000 0x160000 littlefs_backup.bin
```

To restore fan/shade/room data after re-flashing LittleFS, save data via API first, flash,
then re-add via API calls.
