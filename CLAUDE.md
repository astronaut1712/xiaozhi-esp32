# CLAUDE.md — AI Assistant Guide for xiaozhi-esp32

This file documents the codebase structure, development conventions, and workflows for AI assistants working on this project.

## Project Overview

**XiaoZhi ESP32** is open-source firmware for building voice-controlled AI assistant devices on ESP32 microcontrollers. It provides a complete stack: audio capture/playback, wake word detection, LLM communication via WebSocket/MQTT, and an on-device UI with LVGL.

- **Version:** 2.2.4
- **Framework:** ESP-IDF 5.5.2+
- **Supported Chips:** ESP32, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-P4
- **Supported Boards:** 95+ hardware variants
- **License:** MIT

---

## Repository Structure

```
xiaozhi-esp32/
├── main/                    # All application source code
│   ├── application.{cc,h}  # Main app singleton and event loop
│   ├── mcp_server.{cc,h}   # MCP protocol server (JSON-RPC 2.0)
│   ├── main.cc             # Firmware entry point (app_main)
│   ├── audio/              # Audio subsystem (codecs, processors, wake words)
│   ├── display/            # Display subsystem (LCD, OLED, LVGL UI)
│   ├── protocols/          # Network protocols (WebSocket, MQTT+UDP)
│   ├── led/                # LED abstraction layer
│   ├── boards/             # Per-board hardware configurations
│   │   ├── common/         # Shared board components (Button, OTA, etc.)
│   │   └── [board-name]/   # One directory per supported board
│   ├── assets/             # Asset data and language config
│   ├── Kconfig.projbuild   # All Kconfig configuration options
│   ├── idf_component.yml   # Component manager dependencies
│   └── CMakeLists.txt      # Main build configuration
├── docs/                   # Developer documentation
│   ├── custom-board.md     # Guide for adding new board support
│   ├── code_style.md       # C++ style guide
│   ├── mcp-protocol.md     # MCP protocol specification
│   ├── websocket.md        # WebSocket protocol details
│   └── mqtt-udp.md         # MQTT+UDP protocol details
├── scripts/                # Build and utility scripts
│   ├── release.py          # Primary build/release script
│   └── audio_debug_server.py
├── partitions/             # Flash partition table configurations
│   ├── v1/                 # Legacy partition scheme
│   └── v2/                 # Current partition scheme (with assets)
├── .github/workflows/      # CI/CD (GitHub Actions)
├── CMakeLists.txt          # Root CMake config (version, project name)
├── sdkconfig.defaults      # Base SDK configuration
├── sdkconfig.defaults.*    # Target-specific SDK configs
└── .clang-format           # Code formatting rules
```

---

## Build System

### Prerequisites

- ESP-IDF v5.5.2 (use Docker image `espressif/idf:v5.5.2` for CI)
- Python 3 (for `scripts/release.py`)

### Build Commands

```bash
# Set up ESP-IDF environment first
source $IDF_PATH/export.sh

# Build a specific board
python scripts/release.py [board-directory-name]

# Build with a specific variant name
python scripts/release.py [board] --name [variant-name]

# List all available boards as JSON
python scripts/release.py --list-boards --json

# Format all source files
find main -iname "*.h" -o -iname "*.cc" | xargs clang-format -i
```

The build output is `build/merged-binary.bin`.

### Key CMake Settings

- `MINIMAL_BUILD ON` — only include components actually used
- `-Wno-missing-field-initializers` — suppresses a common ESP-IDF warning
- Board type detection is Kconfig-driven; board-specific sources are included conditionally

### sdkconfig Hierarchy

Configuration is applied in this order (later overrides earlier):

1. `sdkconfig.defaults` — common defaults for all targets
2. `sdkconfig.defaults.[target]` — chip-specific (e.g., `sdkconfig.defaults.esp32s3`)
3. Board-level Kconfig options (via `Kconfig.projbuild` selections)

### Important sdkconfig Defaults

- C++ exceptions and RTTI are **enabled**
- Default partition table: custom V2 with 16MB flash
- LVGL version: 9.4.0
- FreeRTOS runtime statistics: enabled
- `CONFIG_UART_ISR_IN_IRAM=y` — required for ML307 4G modem stability

---

## Code Style and Conventions

### Formatting

- **Style:** Google C++ style guide (`.clang-format` in root)
- **Indentation:** 4 spaces
- **Line limit:** 100 characters
- **Braces:** Attach style (opening brace on same line)
- **Pointers/references:** Left-aligned (`int* p`, not `int *p`)
- Run `clang-format -i` before committing; CI will check formatting

To disable formatting for a block:
```cpp
// clang-format off
// manually aligned code
// clang-format on
```

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes | PascalCase | `AudioService`, `WebsocketProtocol` |
| Methods | camelCase | `GetInstance()`, `Initialize()` |
| Private member variables | snake_case with trailing `_` | `event_group_`, `protocol_` |
| Constants / macros | `UPPER_SNAKE_CASE` | `MAX_ENCODE_TASKS_IN_QUEUE` |
| Event bits | `MAIN_EVENT_*` prefix | `MAIN_EVENT_TOGGLE_CHAT` |
| Enum values | `kCamelCase` prefix | `kDeviceStateIdle`, `kAecOff` |
| Log tags | `#define TAG "ComponentName"` | `#define TAG "AudioService"` |
| Config defines | `AUDIO_I2S_GPIO_*`, `DISPLAY_*` | `DISPLAY_WIDTH`, `AUDIO_INPUT_SAMPLE_RATE` |

### File Organization

- Header/implementation split: `.h` and `.cc` (not `.cpp`)
- Each subsystem in its own subdirectory under `main/`
- Board-specific code isolated to `main/boards/[board-name]/`
- Shared board utilities in `main/boards/common/`

### Logging

```cpp
#define TAG "MyComponent"  // at top of .cc file

ESP_LOGI(TAG, "Message: %d", value);
ESP_LOGE(TAG, "Error: %s", esp_err_to_name(err));
ESP_ERROR_CHECK(some_esp_function());
```

---

## Architecture Overview

### Core Patterns

**Singleton** — Used for global service instances:
```cpp
Application& app = Application::GetInstance();
Board& board = Board::GetInstance();
```

**Abstract base classes + per-board/per-chip implementations:**
- `AudioCodec` → `Es8311AudioCodec`, `Es8374AudioCodec`, etc.
- `Protocol` → `WebsocketProtocol`, `MqttProtocol`
- `Display` → `LcdDisplay`, `OledDisplay`, `LvglDisplay`
- `Board` → one class per board in `boards/[name]/`

**Event-driven main loop** — `Application::Run()` blocks on a FreeRTOS event group; all subsystems post events rather than calling Application methods directly.

**Schedule()** — Thread-safe callback dispatch to the main task:
```cpp
Application::GetInstance().Schedule([=]() {
    // runs in main task context
});
```

### Threading Model

| Task | Purpose |
|------|---------|
| Main task (`app_main`) | Event loop, state machine, protocol |
| AudioInputTask | Mic capture, wake word detection, VAD |
| AudioOutputTask | Speaker playback |
| OpusCodecTask | Audio encoding/decoding |
| Network tasks | Managed internally by protocol classes |

Inter-task communication uses FreeRTOS event groups, queues, and mutexes. Never call `Application::Schedule()` from within the main task (deadlock).

### Device State Machine

States: `kDeviceStateStarting` → `kDeviceStateInitializing` → `kDeviceStateIdle` → `kDeviceStateListening` → `kDeviceStateChatting` → `kDeviceStateSleeping`

State transitions are initiated via `Application::SetDeviceState()` or events like `MAIN_EVENT_TOGGLE_CHAT`.

### MCP Protocol Server

`McpServer` implements JSON-RPC 2.0 for AI tool use. Tools are registered with typed properties:
```cpp
mcp_server.AddTool("tool_name", "description", properties,
    [](const cJSON* params) -> McpServer::ToolResult {
        // ...
        return {true, "result"};
    });
```

Property types: `kPropertyTypeBoolean`, `kPropertyTypeInteger` (with min/max), `kPropertyTypeString`.

---

## Adding a New Board

See `docs/custom-board.md` for the full guide. Summary:

1. Create `main/boards/[brand]-[type]/` directory
2. Create `config.h` — all GPIO pin definitions and hardware constants
3. Create `config.json` — build metadata (`target`, `name`, `sdkconfig` overrides)
4. Create `[name]_board.cc` — inherit from `WifiBoard`, `Ml307Board`, or `Board`; override virtual methods for peripherals
5. Register the board in `main/Kconfig.projbuild`
6. Update `main/CMakeLists.txt` to include the new board's sources

**Important:** Never reuse or overwrite an existing board's identity. Each board has a unique OTA firmware channel; mixing them will cause update failures.

### Board Directory Files

```
main/boards/my-board/
├── config.h          # Pin definitions (GPIO, I2C addr, sample rates, etc.)
├── config.json       # {"target": "esp32s3", "name": "my-board", "builds": [...]}
├── my_board.cc       # Board class implementation
└── README.md         # Hardware description and wiring notes
```

### config.json Structure

```json
{
  "target": "esp32s3",
  "builds": [
    {
      "name": "my-board",
      "sdkconfig": ["CONFIG_FOO=y"]
    }
  ]
}
```

---

## CI/CD

GitHub Actions workflow: `.github/workflows/build.yml`

**Triggers:** Push to `main` or `ci/*` branches; PRs targeting `main`.

**Smart build selection:**
- **Push to main:** builds all board variants
- **Pull request:** only builds boards whose files changed; if anything outside `main/boards/` changed, builds all boards

**Build container:** `espressif/idf:v5.5.2`

**Build command per board:**
```bash
source $IDF_PATH/export.sh
python scripts/release.py [board] --name [name]
```

**Artifact:** `build/merged-binary.bin` uploaded per board variant.

---

## Key Files Reference

| File | Purpose |
|------|---------|
| `main/main.cc` | `app_main()` entry point |
| `main/application.{cc,h}` | Top-level orchestrator, event loop |
| `main/mcp_server.{cc,h}` | MCP tool server (JSON-RPC 2.0) |
| `main/device_state_machine.{cc,h}` | Device state transitions |
| `main/device_state.h` | `DeviceState` and `ListeningMode` enums |
| `main/audio/audio_service.{cc,h}` | Audio pipeline orchestration |
| `main/audio/audio_codec.{cc,h}` | Audio codec HAL base class |
| `main/protocols/protocol.{cc,h}` | Network protocol base class |
| `main/boards/common/board.{cc,h}` | Board HAL base class |
| `main/display/display.{cc,h}` | Display abstraction |
| `main/ota.{cc,h}` | OTA firmware update |
| `main/settings.{cc,h}` | NVS-backed settings persistence |
| `main/idf_component.yml` | All external component dependencies |
| `main/Kconfig.projbuild` | All Kconfig options for this project |
| `scripts/release.py` | Build and packaging script |

---

## Component Dependencies

Dependencies are declared in `main/idf_component.yml` and managed by the ESP-IDF Component Manager (output in `managed_components/`, gitignored).

Key dependencies:
- `lvgl` 9.4.0 — UI framework
- `esp_lvgl_port` 2.7.0 — LVGL ESP-IDF integration
- `esp-sr` 2.3.0 — Wake word / speech recognition
- `esp_audio_codec` 2.4.1 — Audio codec abstraction
- `esp-ml307` 3.6.4 — 4G modem (ML307) support
- `esp-wifi-connect` 3.1.1 — WiFi provisioning
- `xiaozhi-fonts` 1.6.0 — Custom font packs
- Various LCD/touch controller drivers (board-dependent)

After modifying `idf_component.yml`, run `idf.py update-dependencies` to regenerate `dependencies.lock`.

---

## Common Development Tasks

### Adding an MCP Tool

In `main/mcp_server.cc`, register a new tool:
```cpp
mcp_server_.AddTool("my_tool", "Description of what it does",
    {{"param_name", {McpServer::kPropertyTypeString, "param description"}}},
    [this](const cJSON* params) -> McpServer::ToolResult {
        auto value = McpServer::GetStringParam(params, "param_name");
        // do work
        return {true, "success"};
    });
```

### Adding a New Protocol

1. Inherit from `Protocol` (`main/protocols/protocol.h`)
2. Implement all pure virtual methods
3. Instantiate in `Application::InitializeProtocol()` based on Kconfig

### Modifying Audio Pipeline

The audio pipeline is in `main/audio/`. Key extension points:
- Wake word: implement `WakeWord` interface (`main/audio/wake_word.h`)
- Audio processor: implement `AudioProcessor` interface (`main/audio/audio_processor.h`)
- Codec: inherit from `AudioCodec` (`main/audio/audio_codec.h`)

### Adding Display Support

Inherit from `Display` (`main/display/display.h`) or `LvglDisplay` for LVGL-based displays. Override virtual methods for your display controller. Register in the board's `*_board.cc`.

---

## What to Avoid

- **Do not** push to `main` directly; use feature branches and PRs.
- **Do not** reuse an existing board's `name` or identity in `config.json` — each board has a unique OTA channel.
- **Do not** add `sdkconfig` to version control (it is gitignored); use `sdkconfig.defaults` instead.
- **Do not** commit `managed_components/` or `dependencies.lock` — these are build artifacts.
- **Do not** add platform-specific includes outside the appropriate board or target directories.
- **Do not** call blocking operations from the main event loop task; use `Schedule()` with care.
- Always run `clang-format` before committing C++ changes.
