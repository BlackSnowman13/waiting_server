#!/usr/bin/env python3
"""
tools/bake_status.py

Bakes Minecraft Server List Ping (Status Response, Packet ID 0x00) static slices
ahead-of-time (AOT) into Flash ROM byte arrays for Protocol 776 / Minecraft 26.2.

Features:
- Full 64x64 PNG base64 favicon included
- Zero RAM buffer allocation on Raspberry Pi Pico W
- Dynamic MOTD Line 1 insertion on the fly
- Direct streaming from Flash ROM via non-blocking lwIP tcp_write (zero-copy)
"""

import re
import sys
import json

def encode_varint(val):
    out = bytearray()
    while True:
        temp = val & 0x7F
        val >>= 7
        if val != 0:
            out.append(temp | 0x80)
        else:
            out.append(temp)
            break
    return bytes(out)

def build_packet(version_name, protocol, motd_line1, motd_line2, motd_line2_color, favicon_str):
    json_obj = {
        "version": {
            "name": version_name,
            "protocol": protocol
        },
        "players": {
            "max": 20,
            "online": 0,
            "sample": []
        },
        "description": {
            "text": "",
            "extra": [
                {
                    "text": motd_line1 + "\n",
                    "color": "aqua",
                    "bold": True
                },
                {
                    "text": motd_line2,
                    "color": motd_line2_color
                }
            ]
        },
        "favicon": favicon_str,
        "enforcesSecureChat": False
    }

    json_str = json.dumps(json_obj, separators=(',', ':'))
    json_bytes = json_str.encode('utf-8')

    payload = encode_varint(0x00) + encode_varint(len(json_bytes)) + json_bytes
    framed_packet = encode_varint(len(payload)) + payload
    return framed_packet, json_bytes

def format_c_array(name, data):
    lines = [f"alignas(4) inline constexpr uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        if i + 16 < len(data):
            lines.append(f"    {hex_str},")
        else:
            lines.append(f"    {hex_str}")
    lines.append("};")
    return "\n".join(lines)

def main():
    with open('old/src/main.cpp', 'r') as f:
        old_cpp = f.read()

    m = re.search(r'data:image/png;base64,[A-Za-z0-9+/=]+', old_cpp)
    if not m:
        print("Error: Could not extract favicon from old/src/main.cpp", file=sys.stderr)
        sys.exit(1)
    favicon = m.group(0)

    # Marker used to find the exact split point for dynamic MOTD
    placeholder = "___MOTD_MARKER___"

    # Online JSON split
    _, online_json = build_packet("● Online", 776, placeholder, "● Primary server is ONLINE (Ready to join)", "green", favicon)
    pos_online = online_json.find(placeholder.encode('utf-8'))
    online_prefix = online_json[:pos_online]
    online_suffix = online_json[pos_online + len(placeholder):]

    # Offline JSON split
    _, offline_json = build_packet("● Sleeping", 776, placeholder, "● Primary server is sleeping (Join to wake)", "gold", favicon)
    pos_offline = offline_json.find(placeholder.encode('utf-8'))
    offline_prefix = offline_json[:pos_offline]
    offline_suffix = offline_json[pos_offline + len(placeholder):]

    header_content = f"""#pragma once
#include <cstdint>
#include <cstddef>

namespace waiting_server {{

// =============================================================================
// Ahead-of-Time (AOT) Baked Minecraft Status Response Slices (ID 0x00)
// Protocol Version: 776 (Minecraft 1.21.4 / 1.21.5 / 26.2)
// Slices stored in Flash ROM for dynamic MOTD assembly without 12KB SRAM buffer.
// =============================================================================

inline constexpr size_t STATUS_PREFIX_ONLINE_SIZE = {len(online_prefix)};
{format_c_array("STATUS_PREFIX_ONLINE", online_prefix)}

inline constexpr size_t STATUS_SUFFIX_ONLINE_SIZE = {len(online_suffix)};
{format_c_array("STATUS_SUFFIX_ONLINE", online_suffix)}

inline constexpr size_t STATUS_PREFIX_OFFLINE_SIZE = {len(offline_prefix)};
{format_c_array("STATUS_PREFIX_OFFLINE", offline_prefix)}

inline constexpr size_t STATUS_SUFFIX_OFFLINE_SIZE = {len(offline_suffix)};
{format_c_array("STATUS_SUFFIX_OFFLINE", offline_suffix)}

}} // namespace waiting_server
"""

    with open('shared/include/server_status.hpp', 'w', encoding='utf-8') as f:
        f.write(header_content)

    print(f"Generated shared/include/server_status.hpp:")
    print(f"  - Online Prefix: {len(online_prefix)} bytes | Suffix: {len(online_suffix)} bytes")
    print(f"  - Offline Prefix: {len(offline_prefix)} bytes | Suffix: {len(offline_suffix)} bytes")

if __name__ == '__main__':
    main()
