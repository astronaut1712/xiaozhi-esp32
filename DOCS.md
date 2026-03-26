# XiaoZhi ESP32 — Developer Documentation

A practical guide covering configuration, dependencies, building, flashing, updating, and adding new features.

---

## Table of Contents

1. [Configuration Guide](#1-configuration-guide)
2. [Dependency Services](#2-dependency-services)
3. [Custom Builds](#3-custom-builds)
4. [Flashing to a Board](#4-flashing-to-a-board)
5. [Updating Firmware](#5-updating-firmware)
6. [Adding New Features](#6-adding-new-features)

---

## 1. Configuration Guide

### How Configuration Works

Configuration is layered — each level overrides the one above it:

| Layer | File | Scope |
|-------|------|-------|
| 1 | `sdkconfig.defaults` | All chips |
| 2 | `sdkconfig.defaults.esp32s3` (etc.) | Per-chip |
| 3 | Board `config.json` → `sdkconfig` list | Per-board build variant |
| 4 | Runtime NVS | User settings (WiFi, OTA URL, etc.) |

**Never commit `sdkconfig`** — it is gitignored and auto-generated. Use `sdkconfig.defaults` for persistent defaults.

### Key Kconfig Options (`main/Kconfig.projbuild`)

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_OTA_URL` | `https://api.tenclass.net/xiaozhi/ota/` | Server endpoint for version check, activation code, and server address discovery |
| `CONFIG_LANGUAGE_*` | `ZH_CN` | Device UI language (25+ supported) |
| `CONFIG_FLASH_DEFAULT_ASSETS` | on | Which asset bundle to flash (default / custom / emote / none) |
| `CONFIG_CUSTOM_ASSETS_FILE` | `assets.bin` | Path or URL to custom asset bundle when using `FLASH_CUSTOM_ASSETS` |

To change any of these at build time, either:
- Edit the `default` value directly in `Kconfig.projbuild`, or
- Add a `sdkconfig` override in the board's `config.json`:
  ```json
  {
    "target": "esp32s3",
    "builds": [
      {
        "name": "my-board",
        "sdkconfig": [
          "CONFIG_OTA_URL=\"https://my-server.com/ota/\"",
          "CONFIG_LANGUAGE_EN_US=y"
        ]
      }
    ]
  }
  ```

### Important Considerations

**OTA URL is the master server address.** The device calls this URL on every boot to:
- Check for a new firmware version
- Receive the WebSocket/MQTT server address and auth token
- Get an activation code if the device is not yet registered

If you self-host, point `CONFIG_OTA_URL` at your own server. The API contract is documented in `docs/websocket.md` and `docs/mqtt-udp.md`.

**Partition scheme.** The default is V2 with a 16 MB flash assumption. If your board has 8 MB flash, set the correct partition table in the board's `config.json`:
```json
"sdkconfig": ["CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions/v2/8m.csv\""]
```
Available tables: `partitions/v2/8m.csv`, `16m.csv`, `16m_c3.csv`, `32m.csv`.

**C++ exceptions and RTTI are enabled.** Do not disable them — the codebase relies on standard C++ features.

**`CONFIG_UART_ISR_IN_IRAM=y`** is required if using the ML307 4G modem. It is already set in `sdkconfig.defaults`.

---

## 2. Dependency Services

The firmware itself is self-contained, but it connects to external services at runtime.

### Required: OTA / Activation Server

The device contacts `CONFIG_OTA_URL` on boot. This server must implement:

- `GET /` — returns JSON with firmware version info, WebSocket/MQTT server address, and auth token
- `POST /activate` — validates a device challenge and completes registration

The official public server is `https://api.tenclass.net/xiaozhi/ota/`. Self-hosted options:

| Project | Language | Link |
|---------|----------|-------|
| xiaozhi-server | Python | Community-maintained, see project README |
| xiaozhi-server-java | Java | Community-maintained |
| xiaozhi-server-go | Go | Community-maintained |

### Required: AI Backend (WebSocket or MQTT)

The device streams audio to an AI backend server over:

- **WebSocket** (simpler, recommended for self-hosting) — see `docs/websocket.md`
- **MQTT + UDP** (lower latency audio, more complex) — see `docs/mqtt-udp.md`

The backend must handle:
- Opus-encoded audio uplink (16 kHz, mono, 60 ms frames)
- Speech-to-text (STT), LLM inference, text-to-speech (TTS)
- Returning Opus-encoded audio downlink (24 kHz)
- Optional: MCP tool calls for device control

### Optional: Asset Server

Custom wake word models, fonts, sounds, and emoji packs are served from a CDN/HTTP server and stored in the device's `assets` partition (8 MB on 16 MB flash). The asset URL is returned by the OTA server response. You can build a custom asset bundle with `scripts/spiffs_assets/build.py`.

### Network Requirements

| Protocol | Port | Notes |
|----------|------|-------|
| HTTPS | 443 | OTA server |
| WSS | 443 | WebSocket AI backend |
| MQTT TLS | 8883 | MQTT backend |
| UDP | varies | MQTT+UDP audio channel |

---

## 3. Custom Builds

### Prerequisites

```bash
# Install ESP-IDF v5.5.2
# Follow https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32/get-started/
source $IDF_PATH/export.sh

# Or use Docker
docker run --rm -v $(pwd):/project -w /project espressif/idf:v5.5.2 bash
```

### Building a Single Board

```bash
# List all available board names
python scripts/release.py --list-boards --json

# Build a specific board (output: build/merged-binary.bin)
python scripts/release.py bread-compact-wifi

# Build with a custom variant name
python scripts/release.py bread-compact-wifi --name my-custom-build
```

### Customizing Before Build

**Change language:**
```json
// in boards/your-board/config.json
"sdkconfig": ["CONFIG_LANGUAGE_EN_US=y"]
```

**Change OTA server URL:**
```json
"sdkconfig": ["CONFIG_OTA_URL=\"https://your-server.com/ota/\""]
```

**Use a custom asset bundle:**
```json
"sdkconfig": [
  "CONFIG_FLASH_CUSTOM_ASSETS=y",
  "CONFIG_CUSTOM_ASSETS_FILE=\"https://your-cdn.com/assets.bin\""
]
```

### Adding a New Board (Summary)

Full guide: `docs/custom-board.md`.

```bash
mkdir main/boards/brand-boardtype
```

Create these files:

**`config.h`** — GPIO pin definitions:
```c
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_GPIO_MCLK      GPIO_NUM_10
// ... all pin definitions
```

**`config.json`** — build metadata:
```json
{
  "target": "esp32s3",
  "builds": [{ "name": "brand-boardtype" }]
}
```

**`brand_boardtype_board.cc`** — inherit from `WifiBoard` (or `Ml307Board` for 4G):
```cpp
class MyBoard : public WifiBoard {
public:
    MyBoard() { /* init peripherals */ }
    AudioCodec* GetAudioCodec() override { return new Es8311AudioCodec(...); }
    Display* GetDisplay() override { return new LcdDisplay(...); }
};
extern "C" void* create_board() { return new MyBoard(); }
```

Then register in `main/Kconfig.projbuild` and `main/CMakeLists.txt`.

> **Warning:** Never reuse an existing board's name. Each name maps to a unique OTA firmware channel — using an existing name will cause OTA to overwrite your device with the wrong firmware.

---

## 4. Flashing to a Board

### First-Time Flash (Full)

The build output is a single merged binary that includes bootloader, partition table, application, and assets.

```bash
# Build first
python scripts/release.py your-board-name

# Flash using esptool (replace /dev/ttyUSB0 with your port)
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x0 build/merged-binary.bin

# Or use idf.py flash (flashes individual partitions)
idf.py -p /dev/ttyUSB0 flash
```

**Tip:** Hold the BOOT button while pressing RESET to enter download mode if the device does not auto-enter.

### Finding the Serial Port

| OS | Typical port |
|----|-------------|
| Linux | `/dev/ttyUSB0` or `/dev/ttyACM0` |
| macOS | `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART` |
| Windows | `COM3`, `COM4`, etc. (check Device Manager) |

### Flash Size Considerations

| Flash size | Partition table |
|------------|----------------|
| 8 MB | `partitions/v2/8m.csv` |
| 16 MB | `partitions/v2/16m.csv` (default) |
| 32 MB | `partitions/v2/32m.csv` |

If you flash a binary built for 16 MB onto an 8 MB device, the device will crash or fail to boot.

### Monitoring Serial Output

```bash
idf.py -p /dev/ttyUSB0 monitor
# or
esptool.py --port /dev/ttyUSB0 --baud 115200 run
```

Use `Ctrl+]` to exit the monitor.

---

## 5. Updating Firmware

### Via OTA (Recommended)

The device automatically checks for firmware updates on every boot by calling the OTA URL. If a new version is found:

1. The device downloads the new firmware to the inactive `ota_1` (or `ota_0`) partition.
2. On success, it reboots and boots from the new partition.
3. If the new firmware fails to boot, the bootloader falls back to the previous partition automatically.

No user action needed — just ensure the OTA server returns the correct firmware URL. The device sends its current version in the `User-Agent` header.

**To force an OTA update via the AI chat**, use the MCP tool (if your server supports it):
```
"Upgrade firmware to version X.X.X"
```

**To change the OTA server URL at runtime** (without reflashing), connect to the device over serial and write to NVS:
```
# Via idf.py monitor console
nvs_set wifi ota_url str https://your-server.com/ota/
```

### Via Flashing Firmware Directly

Re-flash `build/merged-binary.bin` using esptool as described in section 4. This overwrites both OTA partitions and resets to a clean state. Use this when:
- The device is in a boot loop and OTA rollback is not recovering it
- You are changing the partition table (requires erasing flash)

### Erasing Flash (Full Reset)

```bash
esptool.py --port /dev/ttyUSB0 erase_flash
# Then flash fresh
esptool.py --port /dev/ttyUSB0 write_flash 0x0 build/merged-binary.bin
```

This also clears NVS (WiFi credentials, settings, UUID). The device will start from scratch.

### Assets OTA

Assets (wake word models, fonts, sounds) are updated independently from firmware. On boot, the device checks the asset version from the OTA server and downloads updates to the `assets` partition (SPIFFS). Asset updates do not require a reboot.

---

## 6. Adding New Features

This section walks through adding a concrete new feature: **connecting to a Bluetooth speaker for audio output**.

### Example: Bluetooth Speaker Output

#### Step 1 — Understand the audio output interface

Audio output goes through `AudioCodec`. The base class is in `main/audio/audio_codec.h`. The `AudioOutputTask` calls `OutputData()` to write PCM samples to whatever codec is active.

To route audio to Bluetooth instead of (or in addition to) the I2S speaker, you need a custom `AudioCodec` subclass that forwards PCM to a BT A2DP sink.

#### Step 2 — Enable Bluetooth in sdkconfig

Add to your board's `config.json`:
```json
"sdkconfig": [
  "CONFIG_BT_ENABLED=y",
  "CONFIG_BTDM_CTRL_MODE_BLE_ONLY=n",
  "CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y",
  "CONFIG_BT_CLASSIC_ENABLED=y",
  "CONFIG_BT_A2DP_ENABLE=y",
  "CONFIG_BT_SPP_ENABLED=n"
]
```

#### Step 3 — Create the codec class

Create `main/audio/codecs/bt_a2dp_audio_codec.h` and `bt_a2dp_audio_codec.cc`:

```cpp
#include "audio_codec.h"
#include <esp_a2dp_api.h>
#include <esp_bt.h>
#include <esp_bt_main.h>

class BtA2dpAudioCodec : public AudioCodec {
public:
    BtA2dpAudioCodec();
    ~BtA2dpAudioCodec();

    void OutputData(std::vector<int16_t>& data) override;
    // ... implement required virtual methods
};
```

In the `.cc`, initialize the BT stack and A2DP sink on construction, and in `OutputData()` forward the PCM buffer to the A2DP sink callback.

#### Step 4 — Use the codec in your board

In your `*_board.cc`:
```cpp
#include "codecs/bt_a2dp_audio_codec.h"

AudioCodec* MyBoard::GetAudioCodec() override {
    if (audio_codec_ == nullptr) {
        audio_codec_ = new BtA2dpAudioCodec();
    }
    return audio_codec_;
}
```

#### Step 5 — (Optional) Add an MCP tool to control pairing

In `main/mcp_server.cc`:
```cpp
mcp_server_.AddTool("bluetooth.start_pairing",
    "Put the Bluetooth speaker into pairing mode",
    {},
    [this](const cJSON* params) -> McpServer::ToolResult {
        // call your BT codec to start scanning/pairing
        return {true, "Pairing started"};
    });
```

### General Pattern for Any New Feature

| What to add | Where |
|-------------|-------|
| New hardware peripheral (sensor, motor) | New class in `main/boards/common/` or board-specific `*_board.cc` |
| New audio source/sink | Subclass `AudioCodec` in `main/audio/codecs/` |
| New wake word engine | Implement `WakeWord` interface in `main/audio/wake_words/` |
| New network protocol | Subclass `Protocol` in `main/protocols/`, instantiate in `Application::InitializeProtocol()` |
| New display type | Subclass `Display` or `LvglDisplay` in `main/display/` |
| New AI-controllable action | Register with `mcp_server_.AddTool(...)` in `main/mcp_server.cc` |
| New device state | Add to `DeviceState` enum in `main/device_state.h` and handle in `DeviceStateMachine` |
| New Kconfig option | Add to `main/Kconfig.projbuild` under the `Xiaozhi Assistant` menu |

### Thread Safety Rules

- **Never** call `Application::Schedule()` from the main task — it will deadlock.
- **Always** dispatch changes to shared state from non-main tasks via `Schedule()`:
  ```cpp
  Application::GetInstance().Schedule([=]() {
      // safe to access app state here
  });
  ```
- Use `std::mutex` or FreeRTOS primitives for any data shared between the audio tasks and the main task.
