#!/usr/bin/env python3
"""
tools/export_play_packets.py

Extracts all 38 Play-state packets (play_packet_000 through play_packet_037)
from old/include/captured_packets.hpp and writes them into include/play_packets.hpp.
"""

import re
import sys

def main():
    src_file = 'old/include/captured_packets.hpp'
    dst_file = 'shared/include/play_packets.hpp'

    print(f"Reading from {src_file}...")
    with open(src_file, 'r') as f:
        content = f.read()

    packets = []
    # Match play_packet_000 to play_packet_037
    for i in range(38):
        name = f"play_packet_{i:03d}"
        pattern = re.compile(r'inline\s+constexpr\s+uint8_t\s+' + name + r'\[\]\s*=\s*\{([^;]+)\};')
        m = pattern.search(content)
        if not m:
            print(f"Error: {name} not found in {src_file}", file=sys.stderr)
            sys.exit(1)
        body = m.group(1).strip()
        hex_nums = re.findall(r'0x[0-9A-Fa-f]{2}', body)
        packets.append((name, hex_nums))
        print(f"  Extracted {name}: {len(hex_nums)} bytes")

    total_bytes = sum(len(h) for _, h in packets)
    print(f"Total Play packets: {len(packets)} packets, {total_bytes} bytes ({total_bytes/1024:.1f} KB)")

    with open(dst_file, 'w') as f:
        f.write("// Generated from captured_packets.hpp for Minecraft 26.2 (Protocol 776)\n")
        f.write("#pragma once\n\n")
        f.write("#include <cstdint>\n")
        f.write("#include <cstddef>\n")
        f.write("#include \"config_packets.hpp\"\n\n")
        f.write("namespace waiting_server {\n\n")

        for name, hex_nums in packets:
            f.write(f"alignas(4) inline constexpr uint8_t {name}[{len(hex_nums)}] = {{\n")
            for j in range(0, len(hex_nums), 16):
                chunk = hex_nums[j:j+16]
                line = "    " + ", ".join(chunk)
                if j + 16 < len(hex_nums):
                    line += ","
                f.write(line + "\n")
            f.write("};\n\n")

        f.write(f"inline constexpr size_t NUM_PLAY_PACKETS = {len(packets)};\n\n")
        f.write("inline constexpr PacketBlob PLAY_PACKETS[NUM_PLAY_PACKETS] = {\n")
        for name, hex_nums in packets:
            f.write(f"    {{ {name}, {len(hex_nums)} }},\n")
        f.write("};\n\n")
        f.write("} // namespace waiting_server\n")

    print(f"Successfully wrote {dst_file}")

if __name__ == '__main__':
    main()
