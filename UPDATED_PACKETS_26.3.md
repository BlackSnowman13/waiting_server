# Minecraft 26.3 (Protocol 777) Protocol Migration Report

## 1. Overview & Scope

This document details all network protocol updates made to **WaitingServer** to migrate from **Minecraft Java Edition 26.2 (Release Network Protocol 776)** to **Minecraft Java Edition 26.3 (Release Network Protocol 777)**, based on analysis of the decompiled server jar in `old/references/decomiled_26.3`.

- **Release Protocol Version**: `776` $\rightarrow$ `777`
- **World Data Version**: `5023`
- **Target Platform**: Raspberry Pi Pico W (RP2040) / Bare-metal lwIP raw API

---

## 2. Protocol Version Summary

| State / Parameter | 26.2 (Protocol 776) | 26.3 (Protocol 777) | Status / Action |
| :--- | :--- | :--- | :--- |
| **Protocol Number** | `776` | `777` | Updated in `pico_w/include/config.hpp` |
| **Status MOTD JSON** | `"protocol": 776` | `"protocol": 777` | Regenerated via `tools/bake_status.py` in `server_status.hpp` |
| **Player Entity Type ID** | `156` | `159` | Updated in `pico_w/include/entity_sync.hpp` |

---

## 3. Configuration State Packets (Clientbound)

In Minecraft 26.3, two new packets were added to the Clientbound Configuration protocol (`net.minecraft.network.protocol.configuration.ConfigurationProtocols`):
1. **`ClientboundPostEffectsPacket`** was inserted at index **`0x0A` (10)**.
2. **`ClientboundCodeOfConductPacket`** was appended at index **`0x14` (20)**.

This shifted all packets following index 10 by **+1**:

| Packet Name | 26.2 ID (Hex) | 26.2 ID (Dec) | 26.3 ID (Hex) | 26.3 ID (Dec) | Shift | Code Location |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `CLIENTBOUND_COOKIE_REQUEST` | `0x00` | 0 | `0x00` | 0 | 0 | Unchanged |
| `CLIENTBOUND_CUSTOM_PAYLOAD` | `0x01` | 1 | `0x01` | 1 | 0 | Unchanged |
| `CLIENTBOUND_DISCONNECT` | `0x02` | 2 | `0x02` | 2 | 0 | Unchanged |
| `CLIENTBOUND_FINISH_CONFIGURATION` | `0x03` | 3 | `0x03` | 3 | 0 | Unchanged |
| `CLIENTBOUND_KEEP_ALIVE` | `0x04` | 4 | `0x04` | 4 | 0 | Unchanged |
| `CLIENTBOUND_PING` | `0x05` | 5 | `0x05` | 5 | 0 | Unchanged |
| `CLIENTBOUND_RESET_CHAT` | `0x06` | 6 | `0x06` | 6 | 0 | Unchanged |
| `CLIENTBOUND_REGISTRY_DATA` | `0x07` | 7 | `0x07` | 7 | 0 | Unchanged (`config_packet_03`..`30`) |
| `CLIENTBOUND_RESOURCE_PACK_POP` | `0x08` | 8 | `0x08` | 8 | 0 | Unchanged |
| `CLIENTBOUND_RESOURCE_PACK_PUSH` | `0x09` | 9 | `0x09` | 9 | 0 | Unchanged |
| **`CLIENTBOUND_POST_EFFECTS`** | *N/A* | *N/A* | **`0x0A`** | **10** | **NEW** | *Added in 26.3* |
| `CLIENTBOUND_STORE_COOKIE` | `0x0A` | 10 | `0x0B` | 11 | +1 | Shifted |
| `CLIENTBOUND_TRANSFER` | `0x0B` | 11 | `0x0C` | 12 | +1 | Shifted |
| `CLIENTBOUND_UPDATE_ENABLED_FEATURES` | `0x0C` | 12 | `0x0D` | 13 | +1 | Updated (`config_packet_01`) |
| `CLIENTBOUND_UPDATE_TAGS` | `0x0D` | 13 | `0x0E` | 14 | +1 | Updated (`config_packet_31`) |
| `CLIENTBOUND_SELECT_KNOWN_PACKS` | `0x0E` | 14 | `0x0F` | 15 | +1 | Updated (`config_packet_02`) |
| `CLIENTBOUND_CUSTOM_REPORT_DETAILS` | `0x0F` | 15 | `0x10` | 16 | +1 | Shifted |
| `CLIENTBOUND_SERVER_LINKS` | `0x10` | 16 | `0x11` | 17 | +1 | Shifted |
| `CLIENTBOUND_CLEAR_DIALOG` | `0x11` | 17 | `0x12` | 18 | +1 | Shifted |
| `CLIENTBOUND_SHOW_DIALOG` | `0x12` | 18 | `0x13` | 19 | +1 | Shifted |
| **`CLIENTBOUND_CODE_OF_CONDUCT`** | *N/A* | *N/A* | **`0x14`** | **20** | **NEW** | *Added in 26.3* |

### Configuration Packets (Serverbound)
Serverbound Configuration packets are **completely unchanged**:
- `SERVERBOUND_FINISH_CONFIGURATION`: `0x03`
- `SERVERBOUND_SELECT_KNOWN_PACKS`: `0x07`
- `SERVERBOUND_KEEP_ALIVE`: `0x04`

---

## 4. Play State Packets (Clientbound / Game)

In Minecraft 26.3, three new packets were introduced in the Clientbound Play protocol (`net.minecraft.network.protocol.game.GameProtocols.CLIENTBOUND_TEMPLATE`):
1. **`0x25` (37)**: `CLIENTBOUND_ADD_TRANSIENT_BLOCK` (causes a **+1** shift for packet indices 37..82)
2. **`0x53` (83)**: `CLIENTBOUND_POST_EFFECTS` (causes a **+2** shift for packet indices 83..122)
3. **`0x7B` (123)**: `CLIENTBOUND_SWING_ANIMATION` (causes a **+3** shift for packet indices $\ge 123$)

*(Note: Index `0x00` is reserved for `CLIENTBOUND_BUNDLE_DELIMITER`).*

### Full Mapping of Packets Used in WaitingServer

| Packet Name / Purpose | 26.2 ID (Hex) | 26.2 ID (Dec) | 26.3 ID (Hex) | 26.3 ID (Dec) | Shift | Affected Files / Implementation |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `CLIENTBOUND_ADD_ENTITY` | `0x01` | 1 | `0x01` | 1 | 0 | `pico_w/include/entity_sync.hpp` (Player spawn) |
| `CLIENTBOUND_CHUNK_BATCH_FINISHED` | `0x0B` | 11 | `0x0B` | 11 | 0 | `shared/include/play_packets.hpp` (`play_packet_037`) |
| `CLIENTBOUND_CHUNK_BATCH_START` | `0x0C` | 12 | `0x0C` | 12 | 0 | `shared/include/play_packets.hpp` (`play_packet_027`) |
| `CLIENTBOUND_COMMANDS` | `0x10` | 16 | `0x10` | 16 | 0 | `shared/include/play_packets.hpp` (`play_packet_006`) |
| `CLIENTBOUND_CONTAINER_SET_CONTENT` | `0x12` | 18 | `0x12` | 18 | 0 | `shared/include/play_packets.hpp` (`play_packet_020`) |
| `CLIENTBOUND_ENTITY_EVENT` | `0x22` | 34 | `0x22` | 34 | 0 | `shared/include/play_packets.hpp` (`play_packet_005`) |
| **`CLIENTBOUND_ADD_TRANSIENT_BLOCK`** | *N/A* | *N/A* | **`0x25`** | **37** | **NEW** | *Added in 26.3* |
| `CLIENTBOUND_GAME_EVENT` | `0x26` | 38 | `0x27` | 39 | +1 | `shared/include/play_packets.hpp` (`play_packet_016`) |
| `CLIENTBOUND_INITIALIZE_BORDER` | `0x2B` | 43 | `0x2C` | 44 | +1 | `shared/include/play_packets.hpp` (`play_packet_013`) |
| `CLIENTBOUND_KEEP_ALIVE` | `0x2C` | 44 | `0x2D` | 45 | +1 | `pico_w/waiting_server.cpp` (Dynamic KeepAlive) |
| `CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT` | `0x2D` | 45 | `0x2E` | 46 | +1 | `shared/include/lobby_chunks.hpp` (All 25 chunks), `tools/bake_chunk.py`, `tools/extract_chunks.py` |
| `CLIENTBOUND_LOGIN` (Game Join) | `0x31` | 49 | `0x32` | 50 | +1 | `shared/include/play_packets.hpp` (`play_packet_000`) |
| `CLIENTBOUND_CHANGE_DIFFICULTY` | `0x0A` | 10 | `0x0A` | 10 | 0 | `shared/include/play_packets.hpp` (`play_packet_001`) |
| `CLIENTBOUND_PLAYER_CHAT` | `0x40` | 64 | `0x41` | 65 | +1 | `shared/include/play_packets.hpp` (`play_packet_002`) |
| `CLIENTBOUND_PLAYER_INFO_REMOVE` | `0x45` | 69 | `0x46` | 70 | +1 | `pico_w/include/entity_sync.hpp` (Despawn) |
| `CLIENTBOUND_PLAYER_INFO_UPDATE` | `0x46` | 70 | `0x47` | 71 | +1 | `pico_w/waiting_server.cpp`, `pico_w/include/entity_sync.hpp`, `play_packet_011`, `play_packet_012` |
| `CLIENTBOUND_PLAYER_LOOK_AT` | `0x46` | 70 | `0x47` | 71 | +1 | `shared/include/play_packets.hpp` (`play_packet_011`, `012`) |
| `CLIENTBOUND_PLAYER_ROTATION` | `0x48` | 72 | `0x49` | 73 | +1 | `shared/include/play_packets.hpp` (`play_packet_009`) |
| `CLIENTBOUND_PLAYER_POSITION` | `0x48` | 72 | `0x49` | 73 | +1 | Synchronize Player Position |
| `CLIENTBOUND_RECIPE_BOOK_REMOVE` | `0x4A` | 74 | `0x4B` | 75 | +1 | `shared/include/play_packets.hpp` (`play_packet_008`) |
| `CLIENTBOUND_REMOVE_ENTITIES` | `0x4D` | 77 | `0x4E` | 78 | +1 | `pico_w/include/entity_sync.hpp`, `shared/include/play_packets.hpp` (`play_packet_007`) |
| **`CLIENTBOUND_POST_EFFECTS`** | *N/A* | *N/A* | **`0x53`** | **83** | **NEW** | *Added in 26.3* |
| `CLIENTBOUND_ROTATE_HEAD` | `0x53` | 83 | `0x55` | 85 | +2 | `pico_w/include/entity_sync.hpp` (Spawn & look direction) |
| `CLIENTBOUND_SET_ACTION_BAR_TEXT` | `0x57` | 87 | `0x59` | 89 | +2 | `pico_w/waiting_server.cpp` (Dynamic status HUD), `play_packet_010` |
| `CLIENTBOUND_SET_CHUNK_CACHE_CENTER`| `0x5E` | 94 | `0x60` | 96 | +2 | `pico_w/waiting_server.cpp` (Dynamic view center) |
| `CLIENTBOUND_SET_CHUNK_CACHE_RADIUS`| `0x5E` | 94 | `0x60` | 96 | +2 | `shared/include/play_packets.hpp` (`play_packet_019`) |
| `CLIENTBOUND_SET_DISPLAY_OBJECTIVE` | `0x61` | 97 | `0x63` | 99 | +2 | `shared/include/play_packets.hpp` (`play_packet_015`) |
| `CLIENTBOUND_SET_ENTITY_DATA` (Metadata)| `0x63` | 99 | `0x65` | 101 | +2 | `pico_w/waiting_server.cpp`, `pico_w/include/entity_sync.hpp` (Skin layers & sneak) |
| `CLIENTBOUND_SET_ENTITY_LINK` | `0x63` | 99 | `0x65` | 101 | +2 | `shared/include/play_packets.hpp` (`play_packet_022`) |
| `CLIENTBOUND_SET_HEALTH` | `0x67` | 103 | `0x69` | 105 | +2 | `shared/include/play_packets.hpp` (`play_packet_026`) |
| `CLIENTBOUND_SET_HELD_SLOT` | `0x68` | 104 | `0x6A` | 106 | +2 | `shared/include/play_packets.hpp` (`play_packet_025`) |
| `CLIENTBOUND_SET_OBJECTIVE` | `0x69` | 105 | `0x6B` | 107 | +2 | `shared/include/play_packets.hpp` (`play_packet_003`) |
| `CLIENTBOUND_SET_TITLE_TEXT` | `0x71` | 113 | `0x73` | 115 | +2 | `shared/include/play_packets.hpp` (`play_packet_014`, `021`) |
| **`CLIENTBOUND_SWING_ANIMATION`** | *N/A* | *N/A* | **`0x7B`** | **123** | **NEW** | *Added in 26.3* |
| `CLIENTBOUND_TELEPORT_ENTITY` | `0x7D` | 125 | `0x80` | 128 | +3 | `pico_w/include/entity_sync.hpp` (Player position broadcast) |
| `CLIENTBOUND_TICKING_STATE` | `0x7F` | 127 | `0x82` | 130 | +3 | `shared/include/play_packets.hpp` (`play_packet_017`: expands to 2-byte VarInt `0x82, 0x01`) |
| `CLIENTBOUND_TICKING_STEP` | `0x80` | 128 | `0x83` | 131 | +3 | `shared/include/play_packets.hpp` (`play_packet_018`) |
| `CLIENTBOUND_TRANSFER` | `0x81` | 129 | `0x84` | 132 | +3 | `pico_w/waiting_server.cpp` (Dynamic server redirect) |
| `CLIENTBOUND_UPDATE_ATTRIBUTES` | `0x82` | 130 | `0x85` | 133 | +3 | `shared/include/play_packets.hpp` (`play_packet_024`) |
| `CLIENTBOUND_UPDATE_MOB_EFFECT` | `0x83` | 131 | `0x86` | 134 | +3 | `shared/include/play_packets.hpp` (`play_packet_023`) |
| `CLIENTBOUND_UPDATE_TAGS` | `0x85` | 133 | `0x88` | 136 | +3 | `shared/include/play_packets.hpp` (`play_packet_004`) |

---

## 5. Play State Packets (Serverbound)

In Minecraft 26.3:
- Added `SERVERBOUND_PUNCH` at index `0x2E` (46).
- Removed `SERVERBOUND_SWING` at index `0x3F` (63).

All serverbound packets parsed by `waiting_server` precede index `46` and are **identical**:
- `SERVERBOUND_ACCEPT_TELEPORTATION`: `0x00`
- `SERVERBOUND_CHUNK_BATCH_RECEIVED`: `0x0B` (Updated handler check in `waiting_server.cpp` to explicitly match `0x0B`)
- `SERVERBOUND_KEEP_ALIVE`: `0x1C`
- `SERVERBOUND_MOVE_PLAYER_POS`: `0x1E`
- `SERVERBOUND_MOVE_PLAYER_POS_ROT`: `0x1F`
- `SERVERBOUND_MOVE_PLAYER_ROT`: `0x20`
- `SERVERBOUND_MOVE_PLAYER_STATUS_ONLY`: `0x21`
- `SERVERBOUND_PLAYER_INPUT` (Sneaking): `0x2B`

---

## 6. Entity Type ID Registry Update

In `net.minecraft.world.entity.EntityTypes`, 3 new entity types (`cushion`, `poplar_boat`, `poplar_chest_boat`) were registered before `PLAYER`:

- **26.2 (Protocol 776)**: `PLAYER = 156`
- **26.3 (Protocol 777)**: `PLAYER = 159`

Updated in `pico_w/include/entity_sync.hpp`:
```cpp
spawn_writer.write_varint(159) && // Entity Type: Player (159 in Protocol 777 / 26.3)
```

---

## 7. Modified Codebase Files

1. **`pico_w/include/config.hpp`**:
   - `PROTOCOL_VERSION = 777`
2. **`tools/bake_status.py`**:
   - Updated protocol version field in baked JSON to `777`.
3. **`shared/include/server_status.hpp`**:
   - Regenerated status response slices with `"protocol": 777`.
4. **`tools/bake_config_packets.py`**:
   - Tool to bake all 37 canonical 26.3 configuration packets directly into C++ arrays.
5. **`shared/include/config_packets.hpp`**:
   - Complete migration to Minecraft 26.3 configuration state sequence (37 packets total):
     - `config_packet_00` (Brand): `minecraft:brand` = `"vanilla"` (`0x01`)
     - `config_packet_01` (Features): `minecraft:vanilla` (`0x0D`)
     - `config_packet_02` (Known Packs): `minecraft:core` version `"26.3"` (`0x0F`)
     - `config_packet_03`..`34`: All 32 synchronized registries in 26.3 with `canSkipContents = true`
     - `config_packet_35` (Update Tags): Canonical 26.3 network tags (`0x0E`, 58,758 bytes)
     - `config_packet_36` (Finish Configuration): `0x03`
   - Memory footprint reduced from 862 KB down to 68.8 KB.
6. **`shared/include/play_packets.hpp`**:
   - All 29 baked Play packets updated to 26.3 packet IDs.
   - `play_packet_017` length adjusted from 7 to 8 bytes due to 2-byte VarInt encoding for ID `0x82` (`0x82, 0x01`).
7. **`shared/include/lobby_chunks.hpp`**:
   - All 25 AOT baked lobby chunks updated from ID `0x2D` to `0x2E`.
8. **`tools/bake_chunk.py` & `tools/extract_chunks.py`**:
   - Updated LevelChunkWithLight serialization packet ID from `0x2D` to `0x2E`.
9. **`pico_w/include/entity_sync.hpp`**:
   - Player Info Update: `0x46` $\rightarrow$ `0x47`
   - Player Entity Type: `156` $\rightarrow$ `159`
   - Set Entity Data: `0x63` $\rightarrow$ `0x65`
   - Rotate Head: `0x53` $\rightarrow$ `0x55`
   - Teleport Entity: `0x7D` $\rightarrow$ `0x80`
   - Remove Entities: `0x4D` $\rightarrow$ `0x4E`
   - Player Info Remove: `0x45` $\rightarrow$ `0x46`
10. **`pico_w/waiting_server.cpp`**:
    - Dynamic Player Info Update: `0x46` $\rightarrow$ `0x47`
    - Dynamic Set Chunk Cache Center: `0x5E` $\rightarrow$ `0x60`
    - Dynamic Set Entity Metadata: `0x63` $\rightarrow$ `0x65`
    - Dynamic Play KeepAlive: `0x2C` $\rightarrow$ `0x2D`
    - Dynamic Action Bar Text: `0x57` $\rightarrow$ `0x59`
    - Dynamic Play Transfer: `0x81` $\rightarrow$ `0x84`
    - Chunk Batch Received check: added `0x0B`
    - Boot banner string updated to 26.3

---

## 8. Resolution of Client Crash (`Registry Loading`)

### 8.1 Error Symptom
When connecting a Minecraft 26.3 client, the client crashed during the Configuration state with:
```
java.lang.IllegalStateException: Failed to load registries due to errors
...
minecraft:enchantment/minecraft:bane_of_arthropods: Failed to parse value ... from server
minecraft:trim_material/minecraft:quartz: Failed to parse value ... from server
```

### 8.2 Root Cause Analysis
1. **Known Packs Mismatch**:
   - `config_packet_02` previously advertised `minecraft:core` with version `"26.1.2"`.
   - The Minecraft 26.3 client only recognizes `minecraft:core` version `"26.3"`.
   - Because the versions did not match, the client rejected the known pack (`canSkipContents = false`), forcing it to deserialize server-provided raw NBT definitions for every registry entry.
2. **Schema Incompatibilities in 26.3**:
   - `TrimMaterial`: In 26.2, required an `assets` compound containing `asset_name`. In 26.3, this was replaced with `palette_id: Identifier`.
   - `Enchantment`: In 26.2, item lists used `RegistryCodecs.homogeneousList`. In 26.3, Mojang migrated to `RegistryCodecs.holderSet`.
   - Deserializing 26.1.2 NBT into 26.3 codecs caused immediate parsing exceptions.

### 8.3 Fix & Architecture
1. **Synchronized Registries in 26.3 (32 Registries)**:
   - Minecraft 26.3 increased the number of network-synchronized registries from 28 to 32 (adding `wolf_sound_variant`, `pig_variant`, `pig_sound_variant`, `cat_sound_variant`, `cow_sound_variant`, `cow_variant`, `chicken_sound_variant`, `chicken_variant`, `zombie_nautilus_variant`, `sulfur_cube_archetype`, `block_transformer`, etc.).
2. **Known Packs Protocol**:
   - Updated `config_packet_02` (`0x0F`) to request `minecraft:core` version `"26.3"`.
   - When client acknowledges `minecraft:core:26.3`, contents can be skipped (`canSkipContents = true`).
   - All 32 registry data packets serialize entry IDs with `Optional.empty()` NBT (byte `0x00`).
   - The client loads built-in 26.3 definitions directly from its local resources without deserializing NBT.
3. **ROM Footprint Impact**:
   - Previous configuration packets with full NBT: **862 KB**.
   - New 26.3 configuration packets (32 registries + canonical tags): **68.8 KB**.
   - Net flash memory savings: **~793 KB**, bringing total binary size to ~953 KB (under half of the 2 MB Pico W flash capacity).

---

## 9. Resolution of Client Crash (`clientbound/minecraft:login`)

### 9.1 Error Symptom
After successfully negotiating registries and exiting the Configuration state, the client crashed upon receiving the first Play packet (`clientbound/minecraft:login`, ID `0x32` / `play_packet_000`):
```text
io.netty.handler.codec.DecoderException: Failed to decode packet 'clientbound/minecraft:login'
Caused by: java.lang.IndexOutOfBoundsException: readerIndex(110) + length(1) exceeds writerIndex(110)
    at net.minecraft.network.FriendlyByteBuf.readBoolean(FriendlyByteBuf.java:1058)
    at net.minecraft.network.codec.ByteBufCodecs$1.decode(ByteBufCodecs.java:72)
```

### 9.2 Root Cause Analysis
1. **`CommonPlayerSpawnInfo` Codec Shift**:
   - In 26.2, `previousGameType` was encoded as a single raw signed byte via `output.writeByte(GameType.getNullableId(...))`, with `-1` (`0xFF`) representing `null` (no previous game mode).
   - In 26.3, `CommonPlayerSpawnInfo` switched `previousGameType` to `GameType.OPTIONAL_STREAM_CODEC`, which uses `ByteBufCodecs.OPTIONAL_VAR_INT`:
     - Empty (`Optional.empty()`): encoded as VarInt `0` (`0x00`).
     - Present: encoded as `gameMode.id + 1`.
2. **Byte Misinterpretation**:
   - In `play_packet_000`, byte 103 had the legacy value `0xFF`.
   - In 26.3, the client's Netty decoder read `0xFF` as the start of a multi-byte VarInt, which greedily consumed the subsequent `0x00` (which was intended for `isDebug`).
   - This shifted every subsequent field: `isFlat`, `lastDeathLocation`, `portalCooldown`, `seaLevel`, and `onlineMode`.
   - When the decoder reached the final field (`enforcesSecureChat`, boolean), it attempted to read past the end of the 110-byte buffer at index 110, throwing `IndexOutOfBoundsException`.

### 9.3 Fix Implemented
- In `shared/include/play_packets.hpp`, updated byte 103 of `play_packet_000` from `0xFF` to `0x00`.
- The packet length remains 110 bytes (111 bytes with VarInt length prefix `0x6E`), and perfectly satisfies the 26.3 `ClientboundLoginPacket` stream codec.

---

## 10. Resolution of Client Crash (`clientbound/minecraft:recipe_book_add`)

### 10.1 Error Symptom
During player spawning in the Play state, the client crashed while decoding `clientbound/minecraft:recipe_book_add` (`0x4B` / `play_packet_008`):
```text
io.netty.handler.codec.DecoderException: Failed to decode packet 'clientbound/minecraft:recipe_book_add'
Caused by: java.lang.IllegalArgumentException: No value with id 115
    at net.minecraft.core.IdMap.byIdOrThrow(IdMap.java:16)
    at net.minecraft.network.codec.ByteBufCodecs$32.decode(ByteBufCodecs.java:669)
```

### 10.2 Root Cause Analysis
- `play_packet_008` (Recipe Book Add), `play_packet_004` (Update Recipes), and `play_packet_006` (Commands) contained baked packet blobs from Minecraft 26.2.
- In `ClientboundRecipeBookAddPacket`, recipe display entries reference internal item IDs and recipe display IDs via `IdMap.byIdOrThrow`.
- Minecraft 26.3 added new recipes, items, and recipe displays, altering the numeric IDs. The 26.3 client failed to look up numeric ID `115` from the old 26.2 payload.
- In addition, Minecraft 26.3 inserted `CLIENTBOUND_POST_EFFECTS` at index `0x53`, shifting all play packet indices $\ge 83$.

### 10.3 Fix Implemented
1. **Tool Created**: Added `tools/bake_play_packets.py` to bake all 28 Play-state packets directly from official Minecraft 26.3 server captures.
2. **Canonical 26.3 Play Packets**:
   - `play_packet_004` (Update Recipes): Updated to canonical 26.3 recipes (3,765 bytes).
   - `play_packet_006` (Commands): Updated to canonical 26.3 commands root (268 bytes, down from 30,242 bytes!).
   - `play_packet_007` (Recipe Book Settings): Updated to canonical 26.3 settings (10 bytes).
   - `play_packet_008` (Recipe Book Add): Updated to canonical 26.3 recipe entries (173 bytes).
   - `play_packet_020` (Post Effects, `0x53`): Inserted canonical 26.3 post effects packet (3 bytes).
   - `play_packet_021`..`027`: Aligned to canonical 26.3 sequence.
3. **Flash Footprint Impact**:
   - `shared/include/play_packets.hpp` shrunk from **217.5 KB** down to **37 KB** (a **~180 KB flash savings**).
   - Total firmware binary size reduced to **924 KB**.

---

## 11. Resolution of Client Crash (`clientbound/minecraft:level_chunk_with_light`)

### 11.1 Error Symptom
After completing Login and starting the Chunk Batch in the Play state, the client crashed on the very first chunk packet:
```text
io.netty.handler.codec.DecoderException: java.io.IOException: Packet play/clientbound/minecraft:level_chunk_with_light (ClientboundLevelChunkWithLightPacket) was larger than I expected, found 15 bytes extra whilst reading packet clientbound/minecraft:level_chunk_with_light
	at io.netty.handler.codec.ByteToMessageDecoder.callDecode(ByteToMessageDecoder.java:515)
	at io.netty.handler.codec.ByteToMessageDecoder.channelRead(ByteToMessageDecoder.java:296)
```

### 11.2 Root Cause Analysis
1. **`BitSet` Serialization Shift (`long[]` $\rightarrow$ `byte[]`)**:
   - In Minecraft 26.2, `ClientboundLightUpdatePacketData` serialized the four light section masks (`skyYMask`, `blockYMask`, `emptySkyYMask`, `emptyBlockYMask`) using `FriendlyByteBuf.writeBitSet(BitSet)`:
     ```java
     public void writeBitSet(BitSet bitSet) {
         this.writeLongArray(bitSet.toLongArray());
     }
     ```
     This wrote a VarInt count of 64-bit longs, followed by 8 bytes per long (e.g. 9 bytes per mask).
   - In Minecraft 26.3, `ClientboundLightUpdatePacketData` migrated to `ByteBufCodecs.BIT_SET`:
     ```java
     public static final StreamCodec<ByteBuf, BitSet> BIT_SET = new StreamCodec<ByteBuf, BitSet>() {
         @Override
         public BitSet decode(ByteBuf input) {
             return BitSet.valueOf(FriendlyByteBuf.readByteArray(input));
         }
         @Override
         public void encode(ByteBuf output, BitSet value) {
             FriendlyByteBuf.writeByteArray(output, value.toByteArray());
         }
     };
     ```
     This writes a VarInt byte length, followed by the raw byte array (`(highest_bit + 7) // 8` bytes in little-endian order, with no 8-byte padding).
2. **The 15-Byte Discrepancy**:
   - In the pre-baked void chunk (`lobby_chunk_000`), the lighting section contained:
     - `skyYMask` = 0 (1 byte: `0x00`)
     - `blockYMask` = 0 (1 byte: `0x00`)
     - `emptySkyYMask` = 26 bits set (26.2 encoded as VarInt(1) + 8 bytes = 9 bytes: `0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF`)
     - `emptyBlockYMask` = 26 bits set (26.2 encoded as VarInt(1) + 8 bytes = 9 bytes)
     - `skyUpdates` count = 0 (1 byte: `0x00`)
     - `blockUpdates` count = 0 (1 byte: `0x00`)
     Total light payload in 26.2: **22 bytes**.
   - When the 26.3 client decoded this data:
     - `skyYMask`: read `0x00` $\rightarrow$ empty
     - `blockYMask`: read `0x00` $\rightarrow$ empty
     - `emptySkyYMask`: read `0x01` (length 1 byte) $\rightarrow$ read `0x00` (1 byte)
     - `emptyBlockYMask`: read `0x00` (length 0 bytes) $\rightarrow$ empty
     - `skyUpdates`: read `0x00` (count 0)
     - `blockUpdates`: read `0x00` (count 0)
     Total bytes consumed by 26.3 decoder: **7 bytes**.
   - Remaining unread bytes in buffer: $22 - 7 = \mathbf{15\text{ bytes}}$.
   - Netty detected the 15 unread bytes and threw:
     `Packet was larger than I expected, found 15 bytes extra whilst reading packet clientbound/minecraft:level_chunk_with_light`.

### 11.3 Supporting Registry Updates
1. **Biomes Registry Shift (`tools/biomes_777.json`)**:
   - Minecraft 26.3 inserted `minecraft:dappled_forest` at index 8.
   - All subsequent biomes shifted by +1 (e.g. `minecraft:plains` shifted from ID 40 to **ID 41**).
   - Generated canonical `tools/biomes_777.json` (67 entries) directly from 26.3 configuration registry data.
2. **Block States Registry Update (`tools/block_states_777.json`)**:
   - Ran official Minecraft 26.3 data generator (`net.minecraft.data.Main --reports --server`) using `server-26.3.jar`.
   - Generated canonical `tools/block_states_777.json` covering all 1,286 blocks and their state property combinations.

### 11.4 Fix Implemented
1. **BitSet Serialization Helper**:
   - Updated `encode_bitset` in both `tools/extract_chunks.py` and `tools/bake_chunk.py`:
     ```python
     def encode_bitset(mask: int) -> bytes:
         """
         Serializes a BitSet according to Minecraft 26.3 (ByteBufCodecs.BIT_SET):
         FriendlyByteBuf.writeByteArray(output, value.toByteArray()).
         VarInt length of bytes, followed by bytes in little-endian order.
         """
         if mask == 0:
             return encode_varint(0)
         nbytes = (mask.bit_length() + 7) // 8
         raw_bytes = mask.to_bytes(nbytes, 'little')
         return encode_varint(nbytes) + raw_bytes
     ```
2. **Chunk Extraction Pipeline**:
   - Updated `tools/extract_chunks.py` defaults to use `block_states_777.json` and `biomes_777.json`.
   - Updated plains biome ID in empty void chunk sections from 40 to 41.
   - Regenerated `shared/include/lobby_chunks.hpp` (all 25 chunks: 9 visible terrain + 16 void border padding).
   - Verified that all 25 chunks decode with 0 unread bytes against the 26.3 `ClientboundLevelChunkWithLightPacket.STREAM_CODEC`.
3. **Firmware & Linux Target Verification**:
   - Rebuilt `pico_w/build/waiting_server.bin` (924 KB, 100% success, 0 errors, 0 warnings).
   - Rebuilt `linux/build/waiting_server` (100% success, 0 errors, 0 warnings).
