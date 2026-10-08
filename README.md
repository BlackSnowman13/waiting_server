# WaitingServer (Protocol 776 / Minecraft 26.2)

An ultra-lightweight stub and gatekeeper Minecraft server supporting both **Raspberry Pi Pico W** (RP2040 / RP2350 microcontroller) and **Linux** (x86_64 / ARM / Raspberry Pi OS).

## Overview

1. **Server List Ping & Status**: Responds to multiplayer menu ping requests with server status (online/offline indicator, player counts, MOTD, and 64x64 PNG favicon).
2. **Wake-on-LAN Gate**: When a player connects, broadcasts a UDP Wake-on-LAN (WoL) magic packet to wake the primary Minecraft server.
3. **Physical Holding Lobby**: Spawns the player on a pre-baked floating platform (Y=125) with lightweight chunk streaming and periodic Keep-Alive and Action Bar updates.
4. **Transparent Transfer**: As soon as the primary server boots and opens its TCP port, issues a clientbound `Transfer` packet (`0x81`) to seamlessly redirect the player to the primary server without requiring reconnect.

---

## Directory Structure

```
waiting_server/
├── CMakeLists.txt             # Top-level CMake orchestrator (defaults to Linux, or Pico W with -DPICO_BOARD=pico_w)
├── shared/
│   └── include/               # Protocol 776 packet definitions, chunk data, status JSON, reader/writer
│       ├── config_packets.hpp
│       ├── lobby_chunks.hpp
│       ├── packet_reader.hpp
│       ├── packet_writer.hpp
│       ├── play_packets.hpp
│       └── server_status.hpp
├── pico_w/                    # Raspberry Pi Pico W firmware implementation
│   ├── CMakeLists.txt         # Standalone Pico SDK build configuration
│   ├── pico_sdk_import.cmake  # Pico SDK bootstrap
│   ├── config.txt             # Device & Wi-Fi configuration
│   ├── config.example.txt     # Template configuration
│   ├── include/               # lwIP networking, captive portal, flash config, WoL
│   └── waiting_server.cpp     # Firmware entry point and lwIP event loop
├── linux/                     # Linux daemon implementation
│   ├── CMakeLists.txt         # Standalone C++20 Asio build configuration
│   ├── config.txt             # Target host/port/MAC and listen configuration
│   ├── config.example.txt     # Template configuration
│   ├── include/               # Linux Asio configuration, native WoL, target poller
│   └── src/
│       └── main.cpp           # Linux server daemon
├── tools/                     # Python utilities for baking chunks and status responses
└── PACKETS.md                 # Exhaustive protocol packet specification
```

---

## Building

### Prerequisites

- **For Linux Target**:
  - `cmake` (3.20+)
  - `g++` or `clang++` with C++20 support
  - Internet access on first CMake configure to fetch standalone Asio (header-only)

- **For Pico W Target**:
  - Raspberry Pi Pico SDK (v2.0+)
  - `arm-none-eabi-gcc` toolchain

### 1. Building Linux Daemon

From the repository root:
```bash
cmake -B build -S .
cmake --build build -j$(nproc)
```
The executable is generated at `build/linux/waiting_server` (or `linux/build/waiting_server` if built inside `linux/`).

### 2. Building Pico W Firmware

From the repository root:
```bash
cmake -B build -S . -DPICO_BOARD=pico_w
cmake --build build -j$(nproc)
```
The UF2 binaries are generated in `build/pico_w/`:
- `waiting_server.uf2`
- `waiting_server_configured.uf2` (pre-bundled with flash configuration sector)

---

## Configuration

### Linux (`linux/config.txt`)

```ini
# Primary Minecraft Server Configuration
target_host=192.168.1.100
target_port=25565
target_mac=AA:BB:CC:DD:EE:FF

# Waiting Server Listen Settings
listen_address=0.0.0.0
listen_port=25565

# Target Server Polling Interval (seconds)
poll_interval_seconds=3
```

Run the server:
```bash
./build/linux/waiting_server [path/to/config.txt]
```

### Pico W (`pico_w/config.txt`)

```ini
wifi_ssid=MyHomeWiFi
wifi_password=SecretPassword
target_host=192.168.1.100
target_port=25565
target_mac=AA:BB:CC:DD:EE:FF
```
Flash `waiting_server_configured.uf2` via BOOTSEL drag-and-drop.