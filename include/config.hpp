#pragma once

#include <cstdint>
#include <cstddef>
#include "lobby_chunks.hpp"

namespace waiting_server {

// Wi-Fi Access Point Configuration
inline constexpr const char* WIFI_SSID       = "anbagam";
inline constexpr const char* WIFI_PASSWORD   = "anbagam2023";
inline constexpr uint32_t    WIFI_TIMEOUT_MS = 30000;

// Local Waiting Server Port
inline constexpr uint16_t    SERVER_PORT     = 25565;

// Primary (Target) Server Configuration
inline constexpr const char* TARGET_HOST     = "64-pinned-potato-actual";
inline constexpr uint16_t    TARGET_PORT     = 25565;
inline constexpr const char* TARGET_MAC      = "AA:BB:CC:DD:EE:FF";

// Minecraft Protocol Configuration
inline constexpr int32_t     PROTOCOL_VERSION = 776; // 26.2 (compatible with 26.x)
inline constexpr const char* VERSION_NAME     = "● Pico W Ready";

// Static Client Connection Pool Size
inline constexpr size_t      MAX_CLIENTS      = 4;

// Player Spawn Coordinates in Lobby (Synced with lobby_chunks.hpp)
inline constexpr double      SPAWN_X          = LOBBY_SPAWN_X;
inline constexpr double      SPAWN_Y          = LOBBY_SPAWN_Y;
inline constexpr double      SPAWN_Z          = LOBBY_SPAWN_Z;

} // namespace waiting_server
