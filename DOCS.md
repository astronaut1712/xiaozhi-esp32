# XiaoZhi ESP32 — Developer Documentation

A practical guide covering local development setup, configuration, dependencies, building, flashing, updating, and adding new features.

---

## Table of Contents

1. [Local Development Setup](#1-local-development-setup)
2. [Configuration Guide](#2-configuration-guide)
3. [Dependency Services](#3-dependency-services)
4. [Custom Builds](#4-custom-builds)
5. [Flashing to a Board](#5-flashing-to-a-board)
6. [Updating Firmware](#6-updating-firmware)
7. [Adding New Features](#7-adding-new-features)

---

## 1. Local Development Setup

### Option A — Native ESP-IDF Installation (Recommended for Active Development)

This gives you the fastest build iterations and full IDE integration.

#### 1.1 Install ESP-IDF v5.5.2

```bash
# Install prerequisites (Ubuntu/Debian)
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv \
     cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# Clone ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
git checkout v5.5.2
git submodule update --init --recursive

# Run the installer (installs toolchain, Python venv, etc.)
./install.sh all

# Add to shell (add this line to ~/.bashrc or ~/.zshrc)
alias get_idf='. ~/esp/esp-idf/export.sh'
```

**macOS prerequisites:**
```bash
# Install Homebrew first if not present: https://brew.sh
brew install cmake ninja dfu-util python3 wget

# Install CP2102/CH340 USB-serial drivers if your board uses them
# CP210x: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
# CH340:  https://github.com/WCHSoftGroup/ch34xser_macos
```

> **macOS tip:** Apple Silicon (M1/M2/M3) Macs work fine. The ESP-IDF installer handles the correct architecture automatically.

**Windows:** use the [ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/).

#### 1.2 Clone the Repository

```bash
git clone https://github.com/astronaut1712/xiaozhi-esp32.git
cd xiaozhi-esp32
```

#### 1.3 Activate the Environment and Build

```bash
get_idf   # or: source ~/esp/esp-idf/export.sh

# Install Python dependencies for the build scripts
pip install -r scripts/requirements.txt 2>/dev/null || true

# Build your target board
python scripts/release.py bread-compact-wifi
```

#### 1.4 IDE Setup — VS Code

Install the **ESP-IDF VS Code Extension** (Espressif IDF):

1. Open the repository folder in VS Code
2. Press `Ctrl+Shift+P` → `ESP-IDF: Configure ESP-IDF Extension`
3. Select `USE EXISTING SETUP` and point it at your `~/esp/esp-idf` path
4. The extension provides IntelliSense, build, flash, and monitor commands

Useful extension commands (`Ctrl+Shift+P`):
- `ESP-IDF: Build your project`
- `ESP-IDF: Flash your project`
- `ESP-IDF: Monitor your device`
- `ESP-IDF: Set Espressif device target` — switch between ESP32 / ESP32-S3 / etc.

For code completion to work correctly, the extension generates a `compile_commands.json` after the first build. You may also create `.vscode/c_cpp_properties.json` pointing at the IDF include paths.

#### 1.5 IDE Setup — CLion

1. Open the project folder
2. CLion will detect `CMakeLists.txt` automatically
3. Go to `Settings → Build, Execution, Deployment → CMake`
4. Set the CMake options to include the IDF toolchain file:
   ```
   -DCMAKE_TOOLCHAIN_FILE=$IDF_PATH/tools/cmake/toolchain-esp32s3.cmake
   ```
5. Set `clang-format` in `Settings → Editor → Code Style → C/C++` → `Formatter: clang-format`

---

### Option B — Docker (Recommended for CI Parity / Clean Builds)

No local toolchain installation required. Matches exactly what CI runs.

```bash
# Pull the exact same image used in CI
docker pull espressif/idf:v5.5.2

# Run an interactive build shell
docker run --rm -it \
  -v $(pwd):/project \
  -w /project \
  espressif/idf:v5.5.2 \
  bash

# Inside the container:
source $IDF_PATH/export.sh
python scripts/release.py bread-compact-wifi
```

To also flash from Docker, pass the serial device:
```bash
docker run --rm -it \
  -v $(pwd):/project \
  -w /project \
  --device /dev/ttyUSB0 \
  espressif/idf:v5.5.2 \
  bash
```

---

### Verifying the Setup

After a successful build you should see:
```
build/merged-binary.bin   ← the file to flash
```

Run this to confirm the environment is working:
```bash
python scripts/release.py --list-boards --json
```

This lists all 95+ supported boards as JSON. If it prints a list, the setup is complete.

---

### Serial Port Permissions (Linux)

On Linux, you may get `Permission denied` when accessing the serial port. Fix:
```bash
sudo usermod -aG dialout $USER
# Log out and back in for the change to take effect
```

---

### Useful Development Commands

```bash
# Format all C++ source files (required before committing)
find main -iname "*.h" -o -iname "*.cc" | xargs clang-format -i

# Watch serial output
idf.py -p /dev/ttyUSB0 monitor

# Build + flash + monitor in one command
idf.py -p /dev/ttyUSB0 build flash monitor

# Debug audio issues (streams mic audio to PC over WiFi)
python scripts/audio_debug_server.py
```

---

## 2. Configuration Guide

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

## 3. Dependency Services

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

## 4. Custom Builds

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

## 5. Flashing to a Board

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

## 6. Updating Firmware

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

## 7. Adding New Features

This section walks through adding a concrete new feature: **connecting to a Bluetooth speaker for audio output**.

### Example: Bluetooth Speaker Output

> **Important:** Classic Bluetooth A2DP (required for speaker audio) is only available on the **original ESP32**. ESP32-S3, C3, C6, and P4 are BLE-only and cannot stream audio over A2DP. Check your chip before proceeding.

This feature is already implemented in the codebase:
- `main/audio/codecs/bt_audio_codec.{h,cc}` — `BtAudioCodec` class
- `main/boards/common/bt_speaker_mcp_tool.{h,cc}` — MCP tools for AI control

#### Step 1 — Enable Bluetooth in your board's config.json

The feature is gated by the project option `CONFIG_USE_BT_SPEAKER` (menuconfig → Xiaozhi Assistant), which requires the Classic BT stack options:

```json
{
  "target": "esp32",
  "builds": [
    {
      "name": "my-board-bt",
      "sdkconfig": [
        "CONFIG_BT_ENABLED=y",
        "CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y",
        "CONFIG_BT_CLASSIC_ENABLED=y",
        "CONFIG_BT_A2DP_ENABLE=y",
        "CONFIG_BT_SPP_ENABLED=n",
        "CONFIG_USE_BT_SPEAKER=y"
      ]
    }
  ]
}
```

> **Note:** `CONFIG_USE_BT_SPEAKER` releases BLE controller memory, so it cannot be combined with BluFi WiFi provisioning (`CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING`).

#### Step 2 — Use BtAudioCodec in your board

In `*_board.cc`, replace the existing codec with `BtAudioCodec`. The codec handles I2S microphone input and BT A2DP speaker output together:

```cpp
#include "codecs/bt_audio_codec.h"
#include "boards/common/bt_speaker_mcp_tool.h"

class MyBoard : public WifiBoard {
    BtAudioCodec* bt_codec_ = nullptr;
    BtSpeakerMcpTool* bt_mcp_tool_ = nullptr;

public:
    MyBoard() {
        // I2S mic pins from your config.h
        bt_codec_ = new BtAudioCodec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DIN
        );

        // Register MCP tools so the AI can control the BT speaker
        bt_mcp_tool_ = new BtSpeakerMcpTool(bt_codec_);
        bt_mcp_tool_->Initialize();
    }

    AudioCodec* GetAudioCodec() override {
        return bt_codec_;
    }
};
```

#### Step 3 — Use via AI chat

Once flashed, the AI assistant can control the Bluetooth speaker through voice:

| What you say | MCP tool called |
|---|---|
| "Scan for Bluetooth speakers" | `self.bluetooth.scan` |
| "Show me what Bluetooth devices are nearby" | `self.bluetooth.list` |
| "Connect to the JBL speaker at ab:cd:ef:01:23:45" | `self.bluetooth.connect` |
| "Disconnect the Bluetooth speaker" | `self.bluetooth.disconnect` |

#### How it works internally

The codec reports `output_sample_rate() = 44100`, so `AudioService`'s existing rate converter delivers PCM already at the A2DP sample rate — the codec itself does no resampling. On connection, the codec issues `esp_a2d_media_ctrl(CHECK_SRC_RDY → START)` to begin streaming; when `AudioService` powers the output down after inactivity, `EnableOutput(false)` suspends the media stream so the radio stops transmitting.

```
AudioOutputTask
  └── AudioCodec::OutputData(44.1 kHz mono PCM, resampled by AudioService)
        └── BtAudioCodec::Write()
              ├── Apply volume, convert mono → stereo
              └── Push to FreeRTOS ring buffer (32 KB)
                    └── A2DP data callback (BT stack task)
                          └── Drain ring buffer → send to BT speaker
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
