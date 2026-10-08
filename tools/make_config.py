#!/usr/bin/env python3
"""
tools/make_config.py

Reads config.txt and compiles a 1024-byte config.uf2 (2 x 256-byte flash pages) targeting
the Pico W configuration flash sector (0x101FF000).
Supports FlashConfig Version 2.
Allows configuring Wi-Fi and target server offline via USB drag-and-drop.

Usage:
  python3 tools/make_config.py
  python3 tools/make_config.py -c config.txt -o config.uf2
"""

import struct
import argparse
import os

FLASH_CONFIG_OFFSET = 0x1FF000
FLASH_TARGET_ADDR   = 0x10000000 + FLASH_CONFIG_OFFSET
FLASH_CONFIG_MAGIC  = 0x57414954 # "WAIT"
FLASH_CONFIG_VERSION = 2
RP2040_FAMILY_ID    = 0xe48bff56

AUTH_MODES = {
    'wpa2_mixed': 0x00400004,
    'wpa2-mixed': 0x00400004,
    'mixed': 0x00400004,
    'wpa2_aes': 0x00400002,
    'wpa2-aes': 0x00400002,
    'wpa2': 0x00400002,
    'aes': 0x00400002,
    'wpa_tkip': 0x00200002,
    'wpa-tkip': 0x00200002,
    'tkip': 0x00200002,
    'wpa': 0x00200002,
    'open': 0,
    'none': 0,
}

def compute_checksum(data_bytes):
    sum_val = 0x12345678
    for b in data_bytes:
        sum_val = (((sum_val << 5) & 0xFFFFFFFF) | (sum_val >> 27)) ^ b
    return sum_val & 0xFFFFFFFF

def parse_config(filepath):
    cfg = {
        'wifi_ssid': '',
        'wifi_pass': '',
        'target_host': '64-pinned-potato-actual',
        'target_port': 25565,
        'target_mac': 'AA:BB:CC:DD:EE:FF',
        'hostname': 'WaitingServer',
        'wifi_auth': 0x00400004, # WPA2 Mixed
        'connect_attempts': 3,
        'motd': '❄ WaitingServer ✦ Pico W',
    }
    if not os.path.exists(filepath):
        print(f"Warning: {filepath} not found, using defaults.")
        return cfg

    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if '=' in line:
                k, v = line.split('=', 1)
                k = k.strip().lower()
                v = v.strip()
                if k in ('wifi_ssid', 'ssid'):
                    cfg['wifi_ssid'] = v
                elif k in ('wifi_pass', 'wifi_password', 'password', 'pass'):
                    cfg['wifi_pass'] = v
                elif k in ('target_host', 'host'):
                    cfg['target_host'] = v
                elif k in ('target_port', 'port'):
                    cfg['target_port'] = int(v)
                elif k in ('target_mac', 'mac'):
                    cfg['target_mac'] = v
                elif k in ('hostname', 'host_name', 'name'):
                    cfg['hostname'] = v
                elif k in ('wifi_auth', 'auth', 'auth_mode'):
                    v_lower = v.lower()
                    if v_lower in AUTH_MODES:
                        cfg['wifi_auth'] = AUTH_MODES[v_lower]
                    else:
                        try:
                            cfg['wifi_auth'] = int(v, 0)
                        except ValueError:
                            cfg['wifi_auth'] = 0x00400004
                elif k in ('connect_attempts', 'attempts', 'retries'):
                    try:
                        cfg['connect_attempts'] = max(1, min(5, int(v)))
                    except ValueError:
                        cfg['connect_attempts'] = 3
                elif k in ('motd', 'motd_line1', 'description'):
                    cfg['motd'] = v
    return cfg

def build_payload(cfg):
    def pack_str(s, length):
        b = s.encode('utf-8')[:length - 1]
        return b + b'\x00' * (length - len(b))

    body = bytearray()
    body.extend(struct.pack('<IH', FLASH_CONFIG_MAGIC, FLASH_CONFIG_VERSION))
    body.extend(pack_str(cfg['wifi_ssid'], 34))
    body.extend(pack_str(cfg['wifi_pass'], 64))
    body.extend(pack_str(cfg['target_host'], 64))
    body.extend(struct.pack('<H', cfg['target_port']))
    body.extend(pack_str(cfg['target_mac'], 18))
    body.extend(pack_str(cfg['hostname'], 32))
    body.extend(struct.pack('<IB', cfg['wifi_auth'], cfg['connect_attempts']))
    body.extend(pack_str(cfg['motd'], 48))
    body.extend(b'\x00' * 3) # reserved[3]

    checksum = compute_checksum(body)
    body.extend(struct.pack('<I', checksum))

    # Pad to 512 bytes (2 x 256-byte flash pages)
    while len(body) < 512:
        body.append(0)
    return bytes(body)

def build_uf2_block(payload_256, target_addr, block_no, total_blocks):
    header = struct.pack(
        '<IIIIIIII',
        0x0A324655,       # Magic 0
        0x9E5D5157,       # Magic 1
        0x00002000,       # Flags (familyID present)
        target_addr,      # Target address
        256,              # Payload size
        block_no,         # Block number
        total_blocks,     # Total blocks
        RP2040_FAMILY_ID  # Family ID
    )
    data = payload_256 + b'\x00' * (476 - len(payload_256))
    footer = struct.pack('<I', 0x0AB16F30) # Magic end
    return header + data + footer

def build_uf2(payload_512, target_addr=FLASH_TARGET_ADDR):
    total_blocks = len(payload_512) // 256
    uf2_out = bytearray()
    for block_no in range(total_blocks):
        chunk = payload_512[block_no * 256 : (block_no + 1) * 256]
        addr = target_addr + (block_no * 256)
        uf2_out.extend(build_uf2_block(chunk, addr, block_no, total_blocks))
    return bytes(uf2_out)

def main():
    parser = argparse.ArgumentParser(description="Generate config.uf2 for Raspberry Pi Pico W.")
    parser.add_argument("-c", "--config", default="config.txt", help="Path to config.txt")
    parser.add_argument("-o", "--output", default="config.uf2", help="Output .uf2 file")
    args = parser.parse_args()

    cfg = parse_config(args.config)
    print(f"Loaded config: SSID='{cfg['wifi_ssid']}', Host='{cfg['target_host']}:{cfg['target_port']}', Hostname='{cfg['hostname']}', Auth=0x{cfg['wifi_auth']:08X}, Retries={cfg['connect_attempts']}")

    payload = build_payload(cfg)
    uf2 = build_uf2(payload)

    with open(args.output, "wb") as f:
        f.write(uf2)
    print(f"Successfully generated {args.output} ({len(uf2)} bytes, {len(uf2)//512} UF2 blocks).")
    print(f"To configure Pico W offline: hold BOOTSEL, plug in USB, and drag {args.output} onto RPI-RP2.")

if __name__ == "__main__":
    main()
