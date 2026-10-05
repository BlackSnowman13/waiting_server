#!/usr/bin/env python3
"""
tools/make_config.py

Reads config.txt and compiles a tiny 512-byte config.uf2 targeting
the Pico W configuration flash sector (0x101FF000).
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
RP2040_FAMILY_ID    = 0xe48bff56

def compute_checksum(data_bytes):
    sum_val = 0x12345678
    for b in data_bytes:
        sum_val = (((sum_val << 5) & 0xFFFFFFFF) | (sum_val >> 27)) ^ b
    return sum_val & 0xFFFFFFFF

def parse_config(filepath):
    cfg = {
        'wifi_ssid': 'anbagam',
        'wifi_pass': 'anbagam2023',
        'target_host': '64-pinned-potato-actual',
        'target_port': 25565,
        'target_mac': 'AA:BB:CC:DD:EE:FF'
    }
    if not os.path.exists(filepath):
        print(f"Warning: {filepath} not found, using defaults.")
        return cfg

    with open(filepath, 'r') as f:
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
    return cfg

def build_payload(cfg):
    # FlashConfig struct:
    # uint32 magic
    # uint16 version
    # char wifi_ssid[64]
    # char wifi_password[64]
    # char target_host[64]
    # uint16 target_port
    # char target_mac[20]
    # uint32 checksum
    
    body = bytearray()
    body.extend(struct.pack('<IH', FLASH_CONFIG_MAGIC, 1))
    
    def pack_str(s, length):
        b = s.encode('utf-8')[:length - 1]
        return b + b'\x00' * (length - len(b))
    
    body.extend(pack_str(cfg['wifi_ssid'], 64))
    body.extend(pack_str(cfg['wifi_pass'], 64))
    body.extend(pack_str(cfg['target_host'], 64))
    body.extend(struct.pack('<H', cfg['target_port']))
    body.extend(pack_str(cfg['target_mac'], 20))
    
    checksum = compute_checksum(body)
    body.extend(struct.pack('<I', checksum))
    
    # Pad to 256 bytes (FLASH_PAGE_SIZE)
    while len(body) < 256:
        body.append(0)
    return bytes(body)

def build_uf2(payload_256, target_addr=FLASH_TARGET_ADDR):
    # 512-byte UF2 block
    header = struct.pack(
        '<IIIIIIII',
        0x0A324655,       # Magic 0
        0x9E5D5157,       # Magic 1
        0x00002000,       # Flags (familyID present)
        target_addr,      # Target address
        256,              # Payload size
        0,                # Block number
        1,                # Total blocks
        RP2040_FAMILY_ID  # Family ID
    )
    data = payload_256 + b'\x00' * (476 - len(payload_256))
    footer = struct.pack('<I', 0x0AB16F30) # Magic end
    return header + data + footer

def main():
    parser = argparse.ArgumentParser(description="Generate config.uf2 for Raspberry Pi Pico W.")
    parser.add_argument("-c", "--config", default="config.txt", help="Path to config.txt")
    parser.add_argument("-o", "--output", default="config.uf2", help="Output .uf2 file")
    args = parser.parse_args()

    cfg = parse_config(args.config)
    print(f"Loaded config: SSID='{cfg['wifi_ssid']}', Host='{cfg['target_host']}:{cfg['target_port']}', MAC='{cfg['target_mac']}'")

    payload = build_payload(cfg)
    uf2 = build_uf2(payload)

    with open(args.output, "wb") as f:
        f.write(uf2)
    print(f"Successfully generated {args.output} ({len(uf2)} bytes).")
    print(f"To configure Pico W offline: hold BOOTSEL, plug in USB, and drag {args.output} onto RPI-RP2.")

if __name__ == "__main__":
    main()
