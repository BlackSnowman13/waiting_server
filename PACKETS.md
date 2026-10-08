# Minecraft Protocol Packet Specification (Protocol 776 / 26.2)

This document provides an exhaustive, wiki-style specification of all Minecraft network packets implemented, transmitted, and handled by the `WaitingServer` on the Raspberry Pi Pico W.

The target game version is **Minecraft Java Edition 26.2** (**Protocol ID: `776`**, maintaining backward compatibility with Protocol `775`).

---

## 1. Network Architecture & Wire Format

### 1.1 Wire Protocol Framing
Every Minecraft packet transmitted over TCP is framed with a variable-length integer (`VarInt`) representing the packet's total byte length (excluding the length prefix itself):

```
+-----------------------------------+-----------------------------------+------------------------------------------+
| Packet Length                     | Packet ID                         | Packet Payload                           |
| VarInt (1–5 bytes)                | VarInt (1–5 bytes)                | (Packet Length - Packet ID Length) bytes |
+-----------------------------------+-----------------------------------+------------------------------------------+
```

* Compression (`Zlib`) is **disabled** (`threshold = -1`).
* Encryption is **disabled** (`online-mode = false`).
* Zero runtime heap allocation: Packets are either streamed directly from Flash ROM via `PacketBlob` or assembled within fixed 2 KB stack/connection buffers (`client->tx_buffer`).

### 1.2 Primitive Data Types
| Data Type | Wire Representation | Description |
| :--- | :--- | :--- |
| **`VarInt`** | 1–5 bytes | Variable-length 32-bit signed integer encoded with 7 bits per byte (MSB `0x80` is continuation flag). |
| **`VarLong`** | 1–10 bytes | Variable-length 64-bit signed integer encoded with 7 bits per byte. |
| **`Boolean`** | 1 byte | `0x00` for `false`, `0x01` for `true`. |
| **`Byte` / `UByte`** | 1 byte | Signed (`-128..127`) or unsigned (`0..255`) 8-bit integer. |
| **`Short` / `UShort`** | 2 bytes | 16-bit signed/unsigned integer in big-endian network byte order. |
| **`Int`** | 4 bytes | 32-bit signed two's complement integer in big-endian order. |
| **`Long`** | 8 bytes | 64-bit signed two's complement integer in big-endian order. |
| **`Float`** | 4 bytes | IEEE 754 single-precision floating point number in big-endian order. |
| **`Double`** | 8 bytes | IEEE 754 double-precision floating point number in big-endian order. |
| **`String`** | `VarInt` length + UTF-8 bytes | Sequence of UTF-8 encoded bytes prefixed with length as a `VarInt`. |
| **`UUID`** | 16 bytes | 128-bit Universally Unique Identifier stored as two 64-bit big-endian integers. |
| **`BitSet`** | `VarInt` count + `long[]` | A bitset serialized as a `VarInt` count of 64-bit longs followed by each `long` in big-endian order. |
| **`PalettedContainer`** | Variable | Compact storage for 3D block states (4096 voxels) or biomes (64 voxels) with local palette and packed bit integers. |

---

## 2. Handshake State (State `0`)

The Handshake state is the initial protocol phase when a TCP connection is established on port `25565`.

### 2.1 Handshake (Serverbound)
* **Packet ID:** `0x00`
* **Direction:** Client $\rightarrow$ Server
* **Source:** Handled in `waiting_server.cpp` (`on_tcp_recv`)
* **Description:** Initiates the connection, declares the client's protocol version, target hostname, and requests the next state.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Protocol Version** | `VarInt` | `776` (or `775`) | Client protocol version. Accepted by server stub. |
| **Server Address** | `String` | `"localhost"` or server IP | Hostname or IP string entered by the player in the multiplayer menu. |
| **Server Port** | `UShort` | `25565` | TCP port client connected to. |
| **Next State** | `VarInt` | `1` (Status) or `2` (Login) | Determines whether the client wants server list ping info (`1`) or wants to join the world (`2`). |

---

## 3. Status State (State `1`)

Used by the Minecraft multiplayer menu to query server status, MOTD, player counts, and ping latency.

### 3.1 Status Request (Serverbound)
* **Packet ID:** `0x00`
* **Direction:** Client $\rightarrow$ Server
* **Source:** Handled in `waiting_server.cpp`
* **Description:** Requests server list ping metadata (MOTD, player sample, version, favicon).

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| *(None)* | *(None)* | *(Empty Payload)* | Empty packet indicating status query. |

### 3.2 Status Response (Clientbound)
* **Packet ID:** `0x00`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated in `waiting_server.cpp` (`send_status_response`)
* **Description:** Transmits JSON metadata describing server version, online players, formatted MOTD, and 64x64 PNG Base64 favicon.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Status JSON** | `String` | JSON payload string | Contains version name (`"● Online"` or `"● Booting..."`), protocol `776`, max players, current online players, formatted text component MOTD, and optional `data:image/png;base64,...` icon. |

### 3.3 Ping (Serverbound)
* **Packet ID:** `0x01`
* **Direction:** Client $\rightarrow$ Server
* **Source:** Handled in `waiting_server.cpp`
* **Description:** Client sends arbitrary 64-bit timestamp integer to measure round-trip network latency.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Payload** | `Long` | Arbitrary client timestamp | 64-bit integer timestamp generated by the client. |

### 3.4 Pong (Clientbound)
* **Packet ID:** `0x01`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated in `waiting_server.cpp`
* **Description:** Echoes the exact 64-bit timestamp back to the client to finalize latency calculation.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Payload** | `Long` | Echoed client timestamp | Identical 64-bit integer received from the client Ping packet. |

---

## 4. Login State (State `2`)

Triggered when the client connects with `Next State = 2` to join the server.

### 4.1 Login Start (Serverbound)
* **Packet ID:** `0x00`
* **Direction:** Client $\rightarrow$ Server
* **Source:** Handled in `waiting_server.cpp`
* **Description:** Transmits the player's username and offline/online UUID to initiate authentication.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Name** | `String` | Player username (e.g. `"Steve"`) | Player username (up to 16 characters). Saved in `client->player_name`. |
| **Player UUID** | `UUID` | 16-byte raw UUID | Unique identifier. Saved in `client->player_uuid`. |

### 4.2 Login Success (Clientbound)
* **Packet ID:** `0x02`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated via `PacketWriter` in `waiting_server.cpp`
* **Description:** Informs the client that authentication was successful and provides the official player UUID and profile details.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **UUID** | `UUID` | `client->player_uuid` | Echoed player UUID (16 bytes). |
| **Username** | `String` | `client->player_name` | Echoed username. |
| **Properties Count** | `VarInt` | `0` | Number of game profile properties (textures/skins). `0` for offline skin fallback. |
| **Strict Error Handling** | `Boolean` | `false` | Instructs client not to terminate session on minor protocol warnings. |

### 4.3 Login Acknowledged (Serverbound)
* **Packet ID:** `0x03`
* **Direction:** Client $\rightarrow$ Server
* **Source:** Handled in `waiting_server.cpp`
* **Description:** Sent by the client to confirm login success. Immediately transitions connection to the **Configuration State**.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| *(None)* | *(None)* | *(Empty Payload)* | Empty packet signaling transition to Configuration state. |

---

## 5. Configuration State (State `3`)

Minecraft 1.20.5+ / 26.2 Configuration State. In WaitingServer, this state is used to transmit required client registries from Flash ROM, negotiate network tags, and transition the player into the physical waiting lobby or directly transfer them.

### 5.1 Finish Configuration (Clientbound)
* **Packet ID:** `0x03`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `config_packet_034` in `include/config_packets.hpp`
* **Description:** Signals to the client that the server has finished streaming all registries, tags, and configuration data.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| *(None)* | *(None)* | `{ 0x01, 0x03 }` | 1-byte payload indicating configuration phase is complete. |

### 5.2 Finish Configuration ACK (Serverbound)
* **Packet ID:** `0x03`
* **Direction:** Client $\rightarrow$ Server
* **Source:** Handled in `waiting_server.cpp` (`on_tcp_recv`)
* **Description:** Client confirms receipt of configuration data. Transitions the connection to the **Play State**.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| *(None)* | *(None)* | *(Empty Payload)* | Empty packet confirming transition to Play state. |

### 5.3 Configuration Update Tags (Clientbound)
* **Packet ID:** `0x0D`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `config_packet_033` (8,582 bytes) in `include/config_packets.hpp`
* **Description:** Transmits tag dictionaries (block tags, item tags, fluid tags, biome tags, entity type tags) required by the client.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Tags Array** | Array | Captured tag registry blob | Encoded tag mappings for Minecraft 26.2. |

### 5.4 Configuration Keep Alive (Clientbound / Serverbound)
* **Packet ID:** `0x04`
* **Direction:** Bidirectional
* **Source:** Handled in `waiting_server.cpp`
* **Description:** Prevents the client from timing out while held in configuration state.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Keep Alive ID** | `Long` | Timestamp / Tick counter | 64-bit integer payload echoed by the client. |

### 5.5 Configuration Transfer (Clientbound)
* **Packet ID:** `0x0B`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated dynamically via `PacketWriter`
* **Description:** Seamlessly redirects the client to another server host and port while in configuration state.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Host** | `String` | Target Hostname (from config) | IP address or domain of primary Minecraft server (e.g. `"192.168.1.100"`). |
| **Port** | `VarInt` | Target Port (`25565`) | TCP port of primary Minecraft server. |

### 5.6 Configuration Registry Catalog (`config_packets.hpp`)
Streamed sequentially to the client from Flash ROM. Each packet has ID `0x07` (`ClientboundRegistryDataPacket`).

| Packet Symbol | Registry Identifier | ROM Size | Description & Purpose |
| :--- | :--- | :--- | :--- |
| `config_packet_000` | `minecraft:dimension_type` | 2,752 B | Overworld, Nether, and End dimension definitions (height limits, light, ambient). |
| `config_packet_001` | `minecraft:worldgen/biome` | 74,488 B | Biome registry (temperatures, sky colors, foliage colors, sound settings). |
| `config_packet_002` | `minecraft:banner_pattern` | 5,422 B | Banner pattern asset IDs and item tags. |
| `config_packet_003` | `minecraft:chat_type` | 2,525 B | Chat formatting rules (system, team, emote). |
| `config_packet_004` | `minecraft:damage_type` | 9,997 B | Damage sources (fall, drown, fire, mob, void). |
| `config_packet_005` | `minecraft:trim_material` | 3,115 B | Armor trim materials (amethyst, diamond, gold, netherite, etc.). |
| `config_packet_006` | `minecraft:trim_pattern` | 5,888 B | Armor trim template patterns (coast, dune, eye, silence, etc.). |
| `config_packet_007` | `minecraft:wolf_variant` | 2,512 B | Wolf coat variants (pale, spotted, snowy, black, ashen, etc.). |
| `config_packet_008` | `minecraft:painting_variant`| 7,208 B | Painting motifs, width, height, and asset titles. |
| `config_packet_009` | `minecraft:jukebox_song` | 4,210 B | Jukebox disc definitions (sound events, duration, comparator outputs). |
| `config_packet_010..032` | Other Vanilla Registries | ~740 KB | Instruments, enchantments, frog variants, cat variants, wolf armor. |

---

## 6. Play State: Lifecycle & Environment Packets

Once the configuration handshake completes, the connection enters Play State. WaitingServer transmits the baseline environment, entity initialization, and terrain before entering the idle holding loop.

### 6.1 `play_packet_000`: Login (Play)
* **Packet ID:** `0x31`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_000[111]` in `include/play_packets.hpp`
* **Description:** Initializes the client player entity, dimension, render distance, and world simulation properties.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Entity ID** | `Int` | Dynamic (`300 + client->index`) | Unique entity ID assigned to the player entity. Injected dynamically in `waiting_server.cpp`. |
| **Is Hardcore** | `Boolean` | `false` | Whether hardcore mode is active (enables permadeath screen). |
| **Dimension Count** | `VarInt` | `3` | Number of dimension identifiers in following array. |
| **Dimensions** | `String[]` | `["minecraft:overworld", "minecraft:the_end", "minecraft:the_nether"]` | Dimension identifiers available on this server. |
| **Max Players** | `VarInt` | `20` | Server max player cap shown in tab menu. |
| **View Distance** | `VarInt` | `3` | Chunk render distance. Expanded from 1 to 3 to retain 5x5 chunk grid in cache. |
| **Simulation Distance** | `VarInt` | `3` | Entity/block ticking distance around player. |
| **Reduced Debug Info** | `Boolean` | `false` | Whether F3 screen hides coordinate details. |
| **Show Death Screen** | `Boolean` | `true` | Enables normal respawn screen on player death. |
| **Limited Crafting** | `Boolean` | `false` | If true, crafting requires recipe unlock. |
| **Dimension Type** | `VarInt` | `0` (Index for overworld) | Index in dimension type registry for current world. |
| **Dimension Name** | `String` | `"minecraft:overworld"` | Current active dimension resource identifier. |
| **Hashed Seed** | `Long` | `0x5713DACF777B4CFA` | First 8 bytes of SHA-256 of world seed (used for biome noise generation). |
| **Game Mode** | `Byte` | `0` (Survival) | Default gamemode for player (`0 = Survival, 1 = Creative, 2 = Adventure, 3 = Spectator`). |
| **Previous Game Mode** | `Byte` | `-1` (None) | Previous gamemode (used for F3+N toggles). |
| **Is Debug** | `Boolean` | `false` | Whether world was created with debug world generation. |
| **Is Flat** | `Boolean` | `false` | Whether world is superflat. |
| **Has Death Location** | `Boolean` | `false` | Recovery compass target location present flag. |
| **Portal Cooldown** | `VarInt` | `0` | Cooldown ticks before player can use a nether portal. |
| **Sea Level** | `VarInt` | `63` | Standard ocean level in overworld. |
| **Enforces Secure Chat**| `Boolean` | `false` | Whether server requires signed Mojang chat keys. |

---

### 6.2 `play_packet_001`: Change Difficulty
* **Packet ID:** `0x0A`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_001[4]`
* **Description:** Sets world difficulty.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Difficulty** | `UByte` | `1` (Normal) | `0 = Peaceful, 1 = Easy, 2 = Normal, 3 = Hard`. |
| **Locked** | `Boolean` | `false` | Whether difficulty setting is locked in options menu. |

---

### 6.3 `play_packet_002`: Player Abilities
* **Packet ID:** `0x40`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_002[11]`
* **Description:** Controls player flight, invulnerability, and movement speed.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Flags** | `Byte` | `0x00` | Bitfield: `0x01` Invulnerable, `0x02` Flying, `0x04` Allow Flight, `0x08` Creative Mode Instabreak. |
| **Flying Speed** | `Float` | `0.05f` | Creative flight velocity multiplier. |
| **Field of View** | `Float` | `0.10f` | Walking FOV modifier multiplier. |

---

### 6.4 `play_packet_003`: Set Held Item Slot
* **Packet ID:** `0x69`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_003[3]`
* **Description:** Sets selected hotbar slot.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Slot** | `VarInt` | `0` | Active hotbar index (`0–8`). Set to first slot. |

---

### 6.5 `play_packet_004`: Update Recipes
* **Packet ID:** `0x85`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_004[2988]`
* **Description:** Sends crafting, smelting, blast furnace, and smoker recipe definitions to enable local client prediction.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Recipe Count** | `VarInt` | `7` | Number of base recipes in batch. |
| **Recipe Data** | Array | Baked recipe structures | Definitions for furnace, smoker, and crafting table. |

---

### 6.6 `play_packet_005`: Entity Event (OP Permissions)
* **Packet ID:** `0x22`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_005[7]`
* **Description:** Triggers entity animations or permission changes. In WaitingServer, sets OP permission level.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Entity ID** | `Int` | Dynamic (`300 + client->index`) | Player entity ID. Overwritten dynamically in `client->tx_buffer`. |
| **Status Event** | `Byte` | `28` (OP Level 4) | `24–28` sets operator permission level (`28 = Permission Level 4`). |

---

### 6.7 `play_packet_006`: Commands
* **Packet ID:** `0x10`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_006[30242]`
* **Description:** Transmits client-side command graph for auto-completion.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Command Graph** | Array | Brigadier command node tree | Captured command structure for client syntax highlighting. |

---

### 6.8 `play_packet_007` & `008`: Recipe Book Settings & Add
* **Packet IDs:** `0x4C` & `0x4A`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_007[10]` & `play_packet_008[169]`
* **Description:** Configures recipe book GUI state and grants base wooden recipes.

| Packet | Field Name | Data Type | Value in Firmware Code | Description |
| :--- | :--- | :--- | :--- | :--- |
| `007` | **Gui Open Flags** | `Byte[]` | `0x00, 0x00` | Open/filtering flags for crafting, furnace, blast, smoker. |
| `008` | **Action** | `VarInt` | `1` (Init) | Initializes unlocked recipes for `minecraft:planks`. |

---

### 6.9 `play_packet_009`: Synchronize Player Position
* **Packet ID:** `0x48`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_009[63]` + dynamic injection in `waiting_server.cpp`
* **Description:** Teleports and positions the player at the lobby spawn point.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Teleport ID** | `VarInt` | `1` | Acknowledgment ID client must echo in `TeleportConfirm`. |
| **X** | `Double` | `waiting_server::LOBBY_SPAWN_X` | Absolute X coordinate on top of center chunk (e.g. `8.5`). Overwritten dynamically. |
| **Y** | `Double` | `waiting_server::LOBBY_SPAWN_Y` | Absolute Y coordinate (e.g. `134.0`). Overwritten dynamically. |
| **Z** | `Double` | `waiting_server::LOBBY_SPAWN_Z` | Absolute Z coordinate (e.g. `8.5`). Overwritten dynamically. |
| **Delta X** | `Double` | `0.0` | Initial velocity vector X. |
| **Delta Y** | `Double` | `0.0` | Initial velocity vector Y. |
| **Delta Z** | `Double` | `0.0` | Initial velocity vector Z. |
| **Yaw** | `Float` | `0.0f` | Rotation yaw in degrees. |
| **Pitch** | `Float` | `0.0f` | Rotation pitch in degrees. |
| **Flags** | `Int` | `0` (All Absolute) | Relative bitmask flags (`0 = absolute positioning`). |

---

### 6.10 `play_packet_010`: Server Data
* **Packet ID:** `0x56`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_010[24]`
* **Description:** Transmits play-state server name and MOTD displayed in F3 debug screen.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **MOTD Text** | `String` | `"A Minecraft Server"` | Text description. |
| **Has Icon** | `Boolean` | `false` | Whether packet includes inline server icon. |

---

### 6.11 `play_packet_011` & `012`: Player Info Update
* **Packet ID:** `0x46`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_011[4]` & dynamic generation in `waiting_server.cpp`
* **Description:** Updates player tab list. Packet 11 clears existing entries; Packet 12 registers the player.

| Packet | Field Name | Data Type | Value in Firmware Code | Description |
| :--- | :--- | :--- | :--- | :--- |
| `011` | **Action Mask** | `Byte` | `0xFF` | All update actions. |
| `011` | **Player Count** | `VarInt` | `0` | Clears player list cache. |
| `012` | **Action Mask** | `Byte` | `0xFF` | All update actions. |
| `012` | **Player Count** | `VarInt` | `1` | Adds 1 entry. |
| `012` | **Player UUID** | `UUID` | `client->player_uuid` | Real player UUID. |
| `012` | **Username** | `String` | `client->player_name` | Real player username. |
| `012` | **Properties** | `VarInt` | `0` | Texture property count (0). |
| `012` | **Chat Session** | `Boolean` | `false` | Public key chat session present flag. |
| `012` | **Gamemode** | `VarInt` | `0` (Survival) | Gamemode shown in tab list. |
| `012` | **Listed** | `Boolean` | `true` | Visible in Tab player list. |
| `012` | **Ping** | `VarInt` | `0` | Latency bar indicator (0 ms). |
| `012` | **Display Name** | `Boolean` | `false` | Custom display name override present flag. |

---

### 6.12 `play_packet_013`: Game Event (Start Waiting for Chunks)
* **Packet ID:** `0x2B`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_013[42]`
* **Description:** Informs the client to display the "Loading terrain..." screen and begin buffering incoming chunks.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Event Type** | `UByte` | `13` (Wait for Chunks) | `13 = Start waiting for chunks` (client holds screen until batch finishes). |
| **Value** | `Float` | `0.0f` | Event parameter. |

---

### 6.13 `play_packet_014`: Set Simulation Distance
* **Packet ID:** `0x71`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_014[33]`
* **Description:** Sets simulation distance.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Simulation Distance**| `VarInt` | `3` | Distance in chunks around player that entities simulate. |

---

### 6.14 `play_packet_015`: Set Default Spawn Position
* **Packet ID:** `0x61`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_015[38]`
* **Description:** Sets world compass target.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Position** | `Long` (Packed) | `(8, 134, 8)` | Packed 64-bit integer coordinate `(X:26b, Z:26b, Y:12b)`. |
| **Angle** | `Float` | `0.0f` | Spawn angle in degrees. |

---

### 6.15 `play_packet_016`–`018`: Chat Completions, Ticking State, Step Tick
* **Packet IDs:** `0x26`, `0x7F`, `0x80`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_016[7]`, `017[7]`, `018[4]`
* **Description:** Sets chat tab-completion rules and standard world tick rate (20 ticks/sec).

| Packet | Field Name | Data Type | Value in Firmware Code | Description |
| :--- | :--- | :--- | :--- | :--- |
| `016` | **Action** | `VarInt` | `0` | Custom chat completion flags. |
| `017` | **Tick Rate** | `Float` | `20.0f` | Standard 20 TPS clock rate. |
| `017` | **Is Frozen** | `Boolean` | `false` | Normal active ticking. |
| `018` | **Tick Steps** | `VarInt` | `0` | Debug step tick count. |

---

### 6.16 `play_packet_019`: Set Chunk Cache Center
* **Packet ID:** `0x5E`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Dynamic generation in `waiting_server.cpp`
* **Description:** Informs the client's internal `ClientChunkCache` of the center chunk coordinate around which chunks are rendered.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Chunk X** | `VarInt` | `waiting_server::LOBBY_CENTER_CHUNK_X` | Center chunk X coordinate (e.g. `0` or `-1`). Injected dynamically. |
| **Chunk Z** | `VarInt` | `waiting_server::LOBBY_CENTER_CHUNK_Z` | Center chunk Z coordinate (e.g. `0`). Injected dynamically. |

---

### 6.17 `play_packet_020`: Initialize World Border
* **Packet ID:** `0x12`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_020[52]`
* **Description:** Initializes world border size and warning distances.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Center X** | `Double` | `0.0` | World border origin X. |
| **Center Z** | `Double` | `0.0` | World border origin Z. |
| **Old Diameter** | `Double` | `60,000,000.0` | Current world border diameter in blocks. |
| **New Diameter** | `Double` | `60,000,000.0` | Target world border diameter in blocks. |
| **Speed** | `VarLong` | `0` | Transition time in milliseconds (`0 = immediate`). |
| **Portal Boundary** | `VarInt` | `29,999,984` | Max coordinate for nether portals. |
| **Warning Time** | `VarInt` | `15` | Seconds of warning tint on approaching border. |
| **Warning Blocks** | `VarInt` | `5` | Distance in blocks to border before warning triggers. |

---

### 6.18 `play_packet_021`: Set Chunk Cache Radius
* **Packet ID:** `0x71`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_021[11]`
* **Description:** Notifies client renderer of active chunk cache radius.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Radius** | `VarInt` | `3` | Render radius in chunks. Set to 3 to accommodate 5x5 grid. |

---

### 6.19 `play_packet_022`–`026`: Entity Data, Attributes, Advancements, Exp, Health
* **Packet IDs:** `0x63`, `0x83`, `0x82`, `0x68`, `0x67`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_022` through `play_packet_026`
* **Description:** Initializes entity attributes, base advancements, and full survival health/hunger bars.

| Packet | Packet Name | Packet ID | Key Fields & Values | Description |
| :--- | :--- | :--- | :--- | :--- |
| `022` | **Set Entity Data** | `0x63` | Entity ID, Metadata end `0xFF` | Initializes entity tracker flags. |
| `023` | **Update Attributes**| `0x83` | `generic.movement_speed = 0.1` | Base walking velocity attribute modifier. |
| `024` | **Update Advancements**| `0x82`| Reset `true`, Root unlocked | Unlocks root advancement tree. |
| `025` | **Set Experience** | `0x68` | Bar `0.0f`, Level `0`, Total `0` | Clears experience bar. |
| `026` | **Set Health** | `0x67` | Health `20.0f`, Food `20`, Sat `5.0f` | Spawns player with full 10 hearts and full hunger. |

---

## 7. Play State: Chunk & Lighting Pipeline

Minecraft 26.2 uses a chunk batching handshake (`ChunkBatchStart` $\rightarrow$ `LevelChunkWithLight` $\rightarrow$ `ChunkBatchFinished`).

### 7.1 `play_packet_027`: Chunk Batch Start
* **Packet ID:** `0x0C`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_027[2]`
* **Description:** Signals the beginning of a chunk stream. The client resets its batch buffer.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| *(None)* | *(None)* | `{ 0x01, 0x0C }` | 1-byte payload signaling batch transmission start. |

---

### 7.2 `LOBBY_CHUNKS[0..24]`: Clientbound Level Chunk With Light Packet
* **Packet ID:** `0x2D`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated by `tools/extract_chunks.py` $\rightarrow$ `include/lobby_chunks.hpp`
* **Description:** Transmits an entire $16 \times 384 \times 16$ vertical column of terrain, block states, biomes, heightmaps, and lighting data.

#### Top-Level Packet Structure
| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Chunk X** | `Int` (32-bit BE) | Chunk grid X (`-2..2`) | Signed 32-bit chunk X coordinate. |
| **Chunk Z** | `Int` (32-bit BE) | Chunk grid Z (`-2..2`) | Signed 32-bit chunk Z coordinate. |
| **Heightmaps** | `CompoundTag` / VarInt Map | 3 Heightmap Arrays | Motion blocking and surface heightmaps. |
| **Section Buffer Size**| `VarInt` | Byte length of sections | Length of serialized 24 vertical sections. |
| **Section Buffer** | `Byte[]` | 24 Section Paletted Containers | Sequential serialization of all sections from $Y=-64$ to $Y=320$. |
| **Block Entities Count**| `VarInt` | `0` | Number of tile entities in chunk (omitted to save memory). |
| **Light Data** | Structure | Sky & Block light masks + updates | Lighting data payload for client light engine. |

---

#### 7.2.1 Sub-Structure: Heightmaps
In Protocol 776, heightmaps are serialized as a `VarInt` count of heightmaps followed by type, long count, and packed 9-bit height values relative to $Y=-64$ ($height + 65$):

| Heightmap Type ID | Name | Packed Array Size | Purpose |
| :--- | :--- | :--- | :--- |
| **`4`** | `MOTION_BLOCKING` | 37 Longs (296 bytes) | Determines collision elevation for falling entities and weather. |
| **`1`** | `WORLD_SURFACE` | 37 Longs (296 bytes) | Highest non-air block for client rendering. |
| **`5`** | `MOTION_BLOCKING_NO_LEAVES`| 37 Longs (296 bytes) | Rain and collision surface ignoring leaf blocks. |

---

#### 7.2.2 Sub-Structure: Chunk Sections (PalettedContainer)
Every chunk spans from $Y = -64$ to $Y = 320$, divided into 24 vertical sections of $16 \times 16 \times 16$ blocks (4,096 blocks each). Each section serializes:

| Field Name | Data Type | Value in Real Chunk | Value in Void Chunk | Description |
| :--- | :--- | :--- | :--- | :--- |
| **Non-Empty Blocks** | `Short` (16-bit BE) | `0..4096` | `0` | Count of non-air blocks in section. |
| **Fluid Count** | `Short` (16-bit BE) | `0..4096` | `0` | Count of fluid-containing blocks in section. |
| **Block States Bits** | `UByte` | `0` or `4..8` | `0` | Bits per block state entry (`0 = single-valued palette`). |
| **Block Palette** | Container | Variable or Single GID | Single `0` (Air) | Local-to-global block state ID mapping. |
| **Block States Data** | `Long[]` | Packed bit array | *(Omitted if bits = 0)* | 4096 packed block indices. |
| **Biome Bits** | `UByte` | `0` or `1..3` | `0` | Bits per biome entry (`0 = single-valued palette`). |
| **Biome Palette** | Container | Single ID (e.g. `40` Plains) | Single ID `40` (Plains) | 64 voxel biome mapping ($4 \times 4 \times 4$). |
| **Biome Data** | `Long[]` | Packed bit array | *(Omitted if bits = 0)* | 64 packed biome indices. |

---

#### 7.2.3 Sub-Structure: Lighting Data (`ClientboundLightUpdatePacketData`)
Controls ambient daylight and artificial block light.

| Field Name | Data Type | Real Chunk Value | Void Border Chunk Value | Description |
| :--- | :--- | :--- | :--- | :--- |
| **Sky Light Mask** | `BitSet` | Bits set for sections with SkyLight | `0` (No bits set) | Identifies sections with 2048-byte daylight arrays in this packet. |
| **Block Light Mask** | `BitSet` | Bits set for sections with BlockLight | `0` (No bits set) | Identifies sections with 2048-byte block light arrays in this packet. |
| **Empty Sky Mask** | `BitSet` | `0xFFF` (Sections 0–11 subterranean) | `0x03FFFFFF` (All 26 sections) | Sections containing zero sky light. |
| **Empty Block Mask** | `BitSet` | Non-illuminated section mask | `0x03FFFFFF` (All 26 sections) | Sections containing zero block light. |
| **Sky Updates Count** | `VarInt` | `1..3` (Only occupied + surface) | `0` | Number of 2048-byte sky arrays included. |
| **Sky Updates Data** | Array of `byte[2048]` | Daylight nibble arrays | *(None)* | 4 bits per voxel sky light level (`0–15`). |
| **Block Updates Count**| `VarInt` | `0..3` | `0` | Number of 2048-byte block light arrays included. |
| **Block Updates Data**| Array of `byte[2048]` | Block light nibble arrays | *(None)* | 4 bits per voxel block light level (`0–15`). |

---

#### 7.2.4 Real Chunks vs. Void Border Chunks Comparison
WaitingServer uses a **hybrid void architecture**: the inner $3 \times 3$ chunks contain real terrain, while the outer 16 perimeter chunks are pure void padding to satisfy client meshing rules.

| Feature | Inner $3 \times 3$ Real Chunks (`LOBBY_CHUNKS[real]`) | Outer 16 Void Border Chunks (`LOBBY_CHUNKS[void]`) |
| :--- | :--- | :--- |
| **Purpose** | Visible physical lobby platform where players spawn and stand. | 1-chunk boundary padding to allow neighbor meshing against the void. |
| **Total Chunks** | 9 chunks ($3 \times 3$ grid centered at `LOBBY_CENTER_CHUNK`). | 16 chunks (1-chunk perimeter ring around the $3 \times 3$). |
| **Average Size** | **~35,000 bytes** per chunk. | **1,123 bytes** per chunk. |
| **Block Content** | Full stone, dirt, grass, snow, trees from Anvil `.mca`. | 100% Air (`block_count = 0` in all 24 sections). |
| **Collision** | Solid physical ground (player walks normally). | Zero collision (pure open air / abyss). |
| **Visibility** | **100% Solid & Visible** (all 8 neighbors are loaded in client cache). | Invisible air (no blocks to render). |
| **Edge Meshing** | Outer faces render against air, cleanly showing the island edge. | N/A (contains no geometry). |
| **Flash ROM Usage** | ~317 KB total. | 17.5 KB total ($16 \times 1,123$ B). |

---

### 7.3 `play_packet_037`: Chunk Batch Finished
* **Packet ID:** `0x0B`
* **Direction:** Server $\rightarrow$ Client
* **Source:** `play_packet_037[3]` in `include/play_packets.hpp`
* **Description:** Signals that the server has finished sending the current chunk batch. The client closes the "Loading terrain..." screen, spawns the player entity, and renders the world.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Batch Size** | `VarInt` | `25` (`0x19`) | Total number of chunk packets sent in this batch (9 real + 16 void = 25). |

---

## 8. Play State: Holding Lobby & Transfer

Once the chunk batch finishes, the player is physically standing on the lobby platform. WaitingServer maintains the connection while checking the primary target server.

### 8.1 Entity Metadata (Final Spawn Skin Layers)
* **Packet ID:** `0x63`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated dynamically via `PacketWriter` in `waiting_server.cpp`
* **Description:** Enables player outer skin layers (cape, jacket, sleeves, pants, hat) and finalizes player visibility.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Entity ID** | `VarInt` | Dynamic (`300 + client->index`) | Player entity ID. |
| **Index** | `UByte` | `16` | Entity metadata index for Player Skin Display Mask. |
| **Type** | `VarInt` | `0` (Byte) | Metadata type identifier (`0 = Byte`). |
| **Value** | `Byte` | `0x7F` (All Layers Enabled) | Bitmask: `0x01` Cape, `0x02` Jacket, `0x04` Left Sleeve, `0x08` Right Sleeve, `0x10` Left Pants, `0x20` Right Pants, `0x40` Hat. |
| **End of Metadata** | `UByte` | `0xFF` | Terminator byte marking end of metadata list. |

---

### 8.2 Play Keep Alive (Clientbound)
* **Packet ID:** `0x2C`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated dynamically via `PacketWriter` in `waiting_server.cpp`
* **Description:** Sent every 5 seconds to keep the player connected in the waiting lobby.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Keep Alive ID** | `Long` | `client->last_keep_alive_id` | Unique 64-bit integer timestamp generated by server. |

---

### 8.3 Play Keep Alive Response (Serverbound)
* **Packet ID:** `0x1C`
* **Direction:** Client $\rightarrow$ Server
* **Source:** Handled in `waiting_server.cpp` (`on_tcp_recv`)
* **Description:** Sent by client in response to server keep-alive.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Keep Alive ID** | `Long` | Echoed 64-bit ID | Must match `client->last_keep_alive_id` or connection times out. |

---

### 8.4 Play Transfer (Clientbound)
* **Packet ID:** `0x81`
* **Direction:** Server $\rightarrow$ Client
* **Source:** Generated dynamically via `PacketWriter` in `waiting_server.cpp` (`send_play_transfer`)
* **Description:** Issued the moment the primary Minecraft server responds to TCP port 25565 polling. Redirects the player to the primary server without disconnecting them to the main menu.

| Field Name | Data Type | Value in Firmware Code | Description & Protocol Meaning |
| :--- | :--- | :--- | :--- |
| **Host** | `String` | `config.target_host` | Hostname or IP of primary game server. |
| **Port** | `VarInt` | `config.target_port` (e.g. `25565`) | TCP port of primary game server. |
