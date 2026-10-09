#!/usr/bin/env python3
"""
tools/extract_chunks.py

Extracts a range of Minecraft chunks (e.g. 3x3 grid) from a single Anvil (.mca)
region file, serializes them ahead-of-time (AOT) into Protocol 777
ClientboundLevelChunkWithLight (0x2E) wire packets, and emits a modular C++ header
'include/lobby_chunks.hpp' ready for compilation into Raspberry Pi Pico W Flash ROM.

Features:
- Pure Python 3, zero external pip dependencies (self-contained binary NBT parser).
- Full 26.3 (Protocol 777) block state mapping with property resolution.
- Hybrid lighting engine (uses MCA light if present, synthesizes top-down sunlight).
- Auto-detects safe spawn coordinates on top of terrain.
"""

import sys
import os
import zlib
import struct
import json
import argparse
import math

###############################################################################
# VarInt & BitSet Serialization Helpers
###############################################################################

def encode_varint(val: int) -> bytes:
    """Encodes an integer into a Minecraft VarInt."""
    uval = val & 0xFFFFFFFF
    out = bytearray()
    while True:
        temp = uval & 0x7F
        uval >>= 7
        if uval != 0:
            temp |= 0x80
        out.append(temp)
        if uval == 0:
            break
    return bytes(out)

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

###############################################################################
# Zero-Dependency Binary NBT Reader
###############################################################################

class NBTReader:
    """Fast, recursive binary NBT parser for Minecraft Anvil chunks."""
    def __init__(self, data: bytes, pos: int = 0):
        self.data = data
        self.pos = pos

    def read_byte(self) -> int:
        val = self.data[self.pos]
        self.pos += 1
        return val

    def read_short(self) -> int:
        val = struct.unpack_from('>h', self.data, self.pos)[0]
        self.pos += 2
        return val

    def read_int(self) -> int:
        val = struct.unpack_from('>i', self.data, self.pos)[0]
        self.pos += 4
        return val

    def read_long(self) -> int:
        val = struct.unpack_from('>q', self.data, self.pos)[0]
        self.pos += 8
        return val

    def read_float(self) -> float:
        val = struct.unpack_from('>f', self.data, self.pos)[0]
        self.pos += 4
        return val

    def read_double(self) -> float:
        val = struct.unpack_from('>d', self.data, self.pos)[0]
        self.pos += 8
        return val

    def read_string(self) -> str:
        length = struct.unpack_from('>H', self.data, self.pos)[0]
        self.pos += 2
        s = self.data[self.pos:self.pos + length].decode('utf-8', errors='replace')
        self.pos += length
        return s

    def read_tag_payload(self, tag_type: int):
        if tag_type == 1:
            return self.read_byte()
        elif tag_type == 2:
            return self.read_short()
        elif tag_type == 3:
            return self.read_int()
        elif tag_type == 4:
            return self.read_long()
        elif tag_type == 5:
            return self.read_float()
        elif tag_type == 6:
            return self.read_double()
        elif tag_type == 7:
            length = self.read_int()
            res = self.data[self.pos:self.pos + length]
            self.pos += length
            return res
        elif tag_type == 8:
            return self.read_string()
        elif tag_type == 9:
            item_type = self.read_byte()
            length = self.read_int()
            return [self.read_tag_payload(item_type) for _ in range(length)]
        elif tag_type == 10:
            compound = {}
            while True:
                tt = self.read_byte()
                if tt == 0:
                    break
                name = self.read_string()
                compound[name] = self.read_tag_payload(tt)
            return compound
        elif tag_type == 11:
            length = self.read_int()
            res = struct.unpack_from(f'>{length}i', self.data, self.pos)
            self.pos += length * 4
            return list(res)
        elif tag_type == 12:
            length = self.read_int()
            res = struct.unpack_from(f'>{length}q', self.data, self.pos)
            self.pos += length * 8
            return list(res)
        else:
            raise ValueError(f"Unknown NBT tag type: {tag_type} at offset {self.pos}")

    def read_root(self) -> dict:
        tag_type = self.read_byte()
        if tag_type != 10:
            raise ValueError(f"Root tag must be Compound (10), got {tag_type}")
        _ = self.read_string()
        return self.read_tag_payload(10)

###############################################################################
# Region (.mca) File Parser
###############################################################################

def read_mca_chunk(region_source: str, chunk_x: int, chunk_z: int) -> dict:
    """
    Reads and decompresses an individual chunk NBT compound from an .mca file.
    Supports either a direct .mca file path or a directory containing multiple region files.
    """
    rx = chunk_x >> 5
    rz = chunk_z >> 5
    expected_filename = f"r.{rx}.{rz}.mca"

    if os.path.isdir(region_source):
        mca_path = os.path.join(region_source, expected_filename)
    elif os.path.isfile(region_source):
        if os.path.basename(region_source) == expected_filename:
            mca_path = region_source
        else:
            # Check if matching region file exists in same directory
            alt_path = os.path.join(os.path.dirname(region_source), expected_filename)
            mca_path = alt_path if os.path.exists(alt_path) else region_source
    else:
        mca_path = region_source

    if not os.path.exists(mca_path):
        return None

    with open(mca_path, 'rb') as f:
        header = f.read(4096)

    # 1024 4-byte entries (3 bytes sector offset, 1 byte sector count)
    idx = ((chunk_x & 31) + (chunk_z & 31) * 32) * 4
    if idx + 4 > len(header):
        return None

    entry = header[idx:idx + 4]
    offset_sectors = (entry[0] << 16) | (entry[1] << 8) | entry[2]
    sector_count = entry[3]

    if offset_sectors == 0 or sector_count == 0:
        return None

    with open(mca_path, 'rb') as f:
        f.seek(offset_sectors * 4096)
        length_bytes = f.read(4)
        if len(length_bytes) < 4:
            return None
        length = struct.unpack('>I', length_bytes)[0]
        if length <= 1:
            return None
        comp_type = f.read(1)[0]
        payload = f.read(length - 1)

    if comp_type == 2:  # zlib
        decomp = zlib.decompress(payload)
    elif comp_type == 1:  # gzip
        decomp = zlib.decompress(payload, wbits=zlib.MAX_WBITS | 16)
    else:
        return None

    reader = NBTReader(decomp)
    return reader.read_root()

###############################################################################
# Chunk Serialization (0x2E ClientboundLevelChunkWithLight)
###############################################################################

def resolve_block_state(block_info: dict, registry: dict) -> int:
    """Resolves an NBT palette entry to a Protocol 777 global block state ID."""
    name = block_info.get("Name", "minecraft:air")
    props = block_info.get("Properties", {})
    entry = registry.get(name)
    if not entry:
        return 0  # air
    if props:
        prop_key = ",".join(f"{k}={v}" for k, v in sorted(props.items()))
        if prop_key in entry.get("states", {}):
            return entry["states"][prop_key]
    return entry.get("default", 0)

def unpack_indices(longs: list, bits: int, count: int) -> list:
    """Unpacks a Minecraft SimpleBitStorage long array into individual integers."""
    if bits == 0 or not longs:
        return [0] * count
    values_per_long = 64 // bits
    mask = (1 << bits) - 1
    indices = []
    for l in longs:
        temp = l & 0xFFFFFFFFFFFFFFFF
        for _ in range(values_per_long):
            indices.append(temp & mask)
            temp >>= bits
            if len(indices) == count:
                return indices
    while len(indices) < count:
        indices.append(0)
    return indices

def pack_indices(indices: list, bits: int) -> list:
    """Packs individual integers into a Minecraft SimpleBitStorage long array."""
    if bits == 0:
        return []
    values_per_long = 64 // bits
    mask = (1 << bits) - 1
    out_longs = []
    val = 0
    shift = 0
    for i, idx in enumerate(indices):
        val |= (idx & mask) << shift
        shift += bits
        if (i + 1) % values_per_long == 0 or i == len(indices) - 1:
            out_longs.append(val)
            val = 0
            shift = 0
    return out_longs

def serialize_chunk_packet(
    chunk_x: int,
    chunk_z: int,
    chunk_nbt: dict,
    block_registry: dict,
    biome_registry: dict
) -> tuple[bytes, int]:
    """
    Serializes a single chunk into a full 0x2E wire packet.
    Returns: (packet_wire_bytes, center_block_top_y)
    """
    # Build 24 sections map (-4 to 19)
    sections_by_y = {}
    if chunk_nbt and "sections" in chunk_nbt:
        for s in chunk_nbt["sections"]:
            raw_y = s.get("Y", 0)
            # Handle unsigned byte representation if needed (-4 = 252)
            sec_y = raw_y if raw_y < 128 else raw_y - 256
            sections_by_y[sec_y] = s

    # 1. Heightmap calculation
    # Track top solid block in each column of the 16x16 chunk
    top_y_grid = [[-64 for _ in range(16)] for _ in range(16)]

    # 2. Section Data Buffer assembly
    section_buf = bytearray()
    section_has_blocks = [False] * 24

    for s_idx in range(24):
        sec_y = s_idx - 4  # -4 to 19
        sec_data = sections_by_y.get(sec_y)

        if not sec_data or "block_states" not in sec_data:
            # Completely empty air section
            section_buf.extend(struct.pack('>hh', 0, 0))  # non-empty=0, fluid=0
            # Block states: bits=0, single-value=0 (air)
            section_buf.append(0)
            section_buf.extend(encode_varint(0))
            # Biomes: bits=0, single-value=plains (40)
            section_buf.append(0)
            section_buf.extend(encode_varint(biome_registry.get("minecraft:plains", 40)))
            continue

        bs = sec_data["block_states"]
        raw_pal = bs.get("palette", [{"Name": "minecraft:air"}])
        global_pal = [resolve_block_state(p, block_registry) for p in raw_pal]

        raw_longs = bs.get("data", [])
        raw_bits = max(4, math.ceil(math.log2(len(global_pal)))) if len(global_pal) > 1 else 0
        indices = unpack_indices(raw_longs, raw_bits, 4096) if raw_bits > 0 else [0] * 4096

        # Update top Y grid and count non-empty blocks
        non_empty = 0
        fluid_count = 0
        for idx_in_sec, pal_idx in enumerate(indices):
            gid = global_pal[pal_idx] if pal_idx < len(global_pal) else 0
            if gid != 0:
                non_empty += 1
                bx = idx_in_sec & 0x0F
                bz = (idx_in_sec >> 4) & 0x0F
                by = (idx_in_sec >> 8) + (sec_y * 16)
                if by > top_y_grid[bz][bx]:
                    top_y_grid[bz][bx] = by

        section_has_blocks[s_idx] = (non_empty > 0)
        section_buf.extend(struct.pack('>hh', non_empty, fluid_count))

        # Write Block States PalettedContainer
        if len(global_pal) <= 1:
            section_buf.append(0)  # bits = 0
            section_buf.extend(encode_varint(global_pal[0] if global_pal else 0))
        else:
            bits = max(4, math.ceil(math.log2(len(global_pal))))
            section_buf.append(bits)
            section_buf.extend(encode_varint(len(global_pal)))
            for gid in global_pal:
                section_buf.extend(encode_varint(gid))
            packed_longs = pack_indices(indices, bits)
            for pl in packed_longs:
                section_buf.extend(struct.pack('>Q', pl))

        # Write Biomes PalettedContainer
        biomes_tag = sec_data.get("biomes", {})
        raw_b_pal = biomes_tag.get("palette", ["minecraft:plains"]) if isinstance(biomes_tag, dict) else ["minecraft:plains"]
        global_b_pal = [biome_registry.get(b, 40) for b in raw_b_pal]
        b_longs = biomes_tag.get("data", []) if isinstance(biomes_tag, dict) else []

        if len(global_b_pal) <= 1:
            section_buf.append(0)  # bits = 0
            section_buf.extend(encode_varint(global_b_pal[0] if global_b_pal else 40))
        else:
            b_bits = max(1, math.ceil(math.log2(len(global_b_pal))))
            section_buf.append(b_bits)
            section_buf.extend(encode_varint(len(global_b_pal)))
            for bgid in global_b_pal:
                section_buf.extend(encode_varint(bgid))
            b_indices = unpack_indices(b_longs, b_bits, 64)
            packed_b = pack_indices(b_indices, b_bits)
            for pbl in packed_b:
                section_buf.extend(struct.pack('>Q', pbl))

    # 3. Heightmaps serialization (MOTION_BLOCKING, WORLD_SURFACE, MOTION_BLOCKING_NO_LEAVES)
    # Pack 256 height values (relative to -64: height + 65) into 37 longs (9 bits per entry)
    hm_entries = []
    for z in range(16):
        for x in range(16):
            hm_entries.append(top_y_grid[z][x] + 65)

    packed_hm = []
    val = 0
    for chunk_start in range(0, 256, 7):
        cell = 0
        for i, idx in enumerate(range(chunk_start, min(chunk_start + 7, 256))):
            cell |= (hm_entries[idx] & 0x1FF) << (i * 9)
        packed_hm.append(cell)
    while len(packed_hm) < 37:
        packed_hm.append(0)

    heightmaps_data = bytearray()
    heightmaps_data.extend(encode_varint(3))  # 3 heightmaps
    for hm_type in (4, 1, 5):  # MOTION_BLOCKING=4, WORLD_SURFACE=1, MOTION_BLOCKING_NO_LEAVES=5
        heightmaps_data.extend(encode_varint(hm_type))
        heightmaps_data.extend(encode_varint(len(packed_hm)))
        for pl in packed_hm:
            heightmaps_data.extend(struct.pack('>Q', pl))

    # 4. Hybrid Lighting Assembly
    # Light sections span indices 0 to 25 (covering Y = -5 to 20)
    sky_mask = 0
    empty_sky_mask = 0
    block_mask = 0
    empty_block_mask = 0
    sky_updates = []
    block_updates = []
    full_daylight = bytes([0xFF] * 2048)

    # Determine highest occupied section
    highest_sec = None
    for s_idx in reversed(range(24)):
        if section_has_blocks[s_idx]:
            highest_sec = s_idx
            break

    if highest_sec is not None:
        highest_l_idx = highest_sec + 1
        max_send_l_idx = min(highest_l_idx + 1, 25)
    else:
        highest_l_idx = 0
        max_send_l_idx = 1

    for l_idx in range(max_send_l_idx + 1):
        sec_y = l_idx - 5  # -5 to 20
        sec_data = sections_by_y.get(sec_y)

        # Sky Light
        if sec_data and "SkyLight" in sec_data and len(sec_data["SkyLight"]) == 2048:
            sky_mask |= (1 << l_idx)
            sky_updates.append(bytes(sec_data["SkyLight"]))
        elif l_idx == max_send_l_idx:
            # Section right above terrain surface: daylight 15
            sky_mask |= (1 << l_idx)
            sky_updates.append(full_daylight)
        else:
            empty_sky_mask |= (1 << l_idx)

        # Block Light
        if sec_data and "BlockLight" in sec_data and len(sec_data["BlockLight"]) == 2048:
            block_mask |= (1 << l_idx)
            block_updates.append(bytes(sec_data["BlockLight"]))
        else:
            empty_block_mask |= (1 << l_idx)

    light_data = bytearray()
    light_data.extend(encode_bitset(sky_mask))
    light_data.extend(encode_bitset(block_mask))
    light_data.extend(encode_bitset(empty_sky_mask))
    light_data.extend(encode_bitset(empty_block_mask))

    light_data.extend(encode_varint(len(sky_updates)))
    for su in sky_updates:
        light_data.extend(encode_varint(len(su)))
        light_data.extend(su)

    light_data.extend(encode_varint(len(block_updates)))
    for bu in block_updates:
        light_data.extend(encode_varint(len(bu)))
        light_data.extend(bu)

    # 5. Full Packet Assembly
    body = bytearray()
    body.extend(encode_varint(0x2E))  # ClientboundLevelChunkWithLightPacket
    body.extend(struct.pack('>ii', chunk_x, chunk_z))
    body.extend(heightmaps_data)
    body.extend(encode_varint(len(section_buf)))
    body.extend(section_buf)
    body.extend(encode_varint(0))  # Block entities count = 0 (omitted per spec)
    body.extend(light_data)

    full_packet = encode_varint(len(body)) + body
    center_top_y = top_y_grid[8][8]  # Center block of chunk
    return bytes(full_packet), center_top_y

def serialize_empty_chunk(chunk_x: int, chunk_z: int) -> bytes:
    """
    Serializes a 100% empty (void / air) chunk packet for Minecraft 26.3 (Protocol 777).
    Used for 1-chunk boundary padding so that visible chunks can mesh fully against the void.
    """
    # 24 empty sections (Y = -4 to 19)
    section_buf = bytearray()
    for _ in range(24):
        section_buf.extend(struct.pack('>hh', 0, 0))  # non-empty = 0, fluid = 0
        section_buf.append(0)                         # block states bits = 0
        section_buf.extend(encode_varint(0))          # air (id = 0)
        section_buf.append(0)                         # biomes bits = 0
        section_buf.extend(encode_varint(41))         # plains (id = 41 in 26.3)

    # 3 heightmaps, each 37 longs of 0
    heightmaps_data = bytearray()
    heightmaps_data.extend(encode_varint(3))
    packed_zeros = bytes(37 * 8)
    for hm_type in (4, 1, 5):
        heightmaps_data.extend(encode_varint(hm_type))
        heightmaps_data.extend(encode_varint(37))
        heightmaps_data.extend(packed_zeros)

    # Light data: all 26 sections marked as empty sky and empty block
    all_empty_mask = (1 << 26) - 1
    light_data = bytearray()
    light_data.extend(encode_bitset(0))              # sky_mask = 0
    light_data.extend(encode_bitset(0))              # block_mask = 0
    light_data.extend(encode_bitset(all_empty_mask)) # empty_sky_mask
    light_data.extend(encode_bitset(all_empty_mask)) # empty_block_mask
    light_data.extend(encode_varint(0))              # num sky updates = 0
    light_data.extend(encode_varint(0))              # num block updates = 0

    body = bytearray()
    body.extend(encode_varint(0x2E))                 # ClientboundLevelChunkWithLightPacket
    body.extend(struct.pack('>ii', chunk_x, chunk_z))
    body.extend(heightmaps_data)
    body.extend(encode_varint(len(section_buf)))
    body.extend(section_buf)
    body.extend(encode_varint(0))                     # block entities count = 0
    body.extend(light_data)

    return bytes(encode_varint(len(body)) + body)

###############################################################################
# Header File Generator
###############################################################################

def write_lobby_chunks_header(
    output_path: str,
    chunks: list[dict],
    spawn_coords: tuple[float, float, float],
    center_chunk: tuple[int, int]
):
    """Writes the modular C++ header 'include/lobby_chunks.hpp'."""
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w") as f:
        f.write("// Generated by tools/extract_chunks.py\n")
        f.write("// Modular AOT Baked Minecraft Lobby Chunks with Void Border for Pico W\n")
        f.write("#pragma once\n\n")
        f.write("#include <cstdint>\n")
        f.write("#include <cstddef>\n")
        f.write('#include "config_packets.hpp"\n\n')
        f.write("namespace waiting_server {\n\n")

        # Center chunk and spawn coordinates
        f.write(f"inline constexpr int32_t LOBBY_CENTER_CHUNK_X = {center_chunk[0]};\n")
        f.write(f"inline constexpr int32_t LOBBY_CENTER_CHUNK_Z = {center_chunk[1]};\n")
        f.write(f"inline constexpr double LOBBY_SPAWN_X = {spawn_coords[0]:.1f};\n")
        f.write(f"inline constexpr double LOBBY_SPAWN_Y = {spawn_coords[1]:.1f};\n")
        f.write(f"inline constexpr double LOBBY_SPAWN_Z = {spawn_coords[2]:.1f};\n\n")

        # Write each chunk byte array
        for i, c in enumerate(chunks):
            cx, cz = c["x"], c["z"]
            data = c["data"]
            kind = "VOID" if c.get("is_void") else "REAL"
            f.write(f"// Chunk ({cx}, {cz}) [{kind}] - Size: {len(data)} bytes\n")
            f.write(f"alignas(4) inline constexpr uint8_t lobby_chunk_{i:03d}[{len(data)}] = {{\n    ")
            for b_idx, b in enumerate(data):
                f.write(f"0x{b:02X}, ")
                if (b_idx + 1) % 16 == 0 and (b_idx + 1) < len(data):
                    f.write("\n    ")
            f.write("\n};\n\n")

        # Array of PacketBlobs
        f.write(f"inline constexpr size_t NUM_LOBBY_CHUNKS = {len(chunks)};\n\n")
        f.write("inline constexpr PacketBlob LOBBY_CHUNKS[NUM_LOBBY_CHUNKS] = {\n")
        for i in range(len(chunks)):
            f.write(f"    {{ lobby_chunk_{i:03d}, sizeof(lobby_chunk_{i:03d}) }},\n")
        f.write("};\n\n")

        f.write("} // namespace waiting_server\n")

    print(f"\n[OK] Generated {output_path} with {len(chunks)} chunks.")

###############################################################################
# Main CLI Entry Point
###############################################################################

def main():
    parser = argparse.ArgumentParser(description="Extract and bake Minecraft chunks with void border padding from an Anvil .mca file.")
    parser.add_argument("--region", default="old/references/minecraft_server_26.2/world/dimensions/minecraft/overworld/region",
                        help="Path to region directory or .mca file")
    parser.add_argument("--center-chunk", nargs=2, type=int, default=[-1, 0], metavar=("X", "Z"),
                        help="Center chunk coordinates (X Z)")
    parser.add_argument("--radius", type=int, default=1,
                        help="Radius of visible chunks from region file (default: 1 -> 3x3 visible grid)")
    parser.add_argument("--no-void-border", action="store_true",
                        help="Disable automatic 1-chunk void border padding")
    parser.add_argument("--spawn-x", type=float, help="Override spawn X coordinate")
    parser.add_argument("--spawn-y", type=float, help="Override spawn Y coordinate")
    parser.add_argument("--spawn-z", type=float, help="Override spawn Z coordinate")
    parser.add_argument("--registry", default="tools/block_states_777.json",
                        help="Path to block states registry JSON")
    parser.add_argument("--biomes", default="tools/biomes_777.json",
                        help="Path to biomes registry JSON")
    parser.add_argument("-o", "--output", default="shared/include/lobby_chunks.hpp",
                        help="Output C++ header file")

    args = parser.parse_args()

    # Load registries
    print(f"Loading block state registry from {args.registry}...")
    with open(args.registry, "r") as f:
        block_reg = json.load(f)

    print(f"Loading biome registry from {args.biomes}...")
    with open(args.biomes, "r") as f:
        biome_reg = json.load(f)

    center_x, center_z = args.center_chunk
    rad = args.radius
    use_void_border = not args.no_void_border
    border_rad = rad + 1 if use_void_border else rad

    visible_dims = f"{2*rad+1}x{2*rad+1}"
    total_dims = f"{2*border_rad+1}x{2*border_rad+1}"
    print(f"Extracting {visible_dims} visible chunk grid centered at ({center_x}, {center_z}) from {args.region}...")
    if use_void_border:
        print(f"Adding 1-chunk void border padding ({total_dims} total chunks) for clean void meshing...")

    chunks = []
    detected_spawn_y = 64
    real_bytes = 0
    void_bytes = 0
    real_count = 0
    void_count = 0

    for dz in range(-border_rad, border_rad + 1):
        for dx in range(-border_rad, border_rad + 1):
            cx = center_x + dx
            cz = center_z + dz
            is_void = use_void_border and (abs(dx) > rad or abs(dz) > rad)
            if not is_void:
                nbt_data = read_mca_chunk(args.region, cx, cz)
                pkt_bytes, top_y = serialize_chunk_packet(cx, cz, nbt_data, block_reg, biome_reg)
                chunks.append({"x": cx, "z": cz, "data": pkt_bytes, "is_void": False})
                real_bytes += len(pkt_bytes)
                real_count += 1
                print(f"  REAL Chunk ({cx:>3}, {cz:>3}): {len(pkt_bytes):>6} bytes | Top Y = {top_y}")
                if cx == center_x and cz == center_z:
                    detected_spawn_y = max(top_y + 1, 64)
            else:
                pkt_bytes = serialize_empty_chunk(cx, cz)
                chunks.append({"x": cx, "z": cz, "data": pkt_bytes, "is_void": True})
                void_bytes += len(pkt_bytes)
                void_count += 1
                print(f"  VOID Chunk ({cx:>3}, {cz:>3}): {len(pkt_bytes):>6} bytes")

    total_bytes = real_bytes + void_bytes

    # Spawn coordinates
    spawn_x = args.spawn_x if args.spawn_x is not None else float(center_x * 16 + 8.5)
    spawn_y = args.spawn_y if args.spawn_y is not None else float(detected_spawn_y)
    spawn_z = args.spawn_z if args.spawn_z is not None else float(center_z * 16 + 8.5)

    print(f"\nReal Chunks ({real_count:>2}) : {real_bytes / 1024:.1f} KB")
    if void_count > 0:
        print(f"Void Chunks ({void_count:>2}) : {void_bytes / 1024:.1f} KB")
    print(f"Total Chunk Packets Size: {total_bytes / 1024:.1f} KB")
    print(f"Player Spawn Point      : X={spawn_x:.1f}, Y={spawn_y:.1f}, Z={spawn_z:.1f}")

    write_lobby_chunks_header(args.output, chunks, (spawn_x, spawn_y, spawn_z), (center_x, center_z))

if __name__ == "__main__":
    main()
