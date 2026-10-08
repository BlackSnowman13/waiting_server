#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/cyw43_arch.h"

namespace waiting_server {

// Dedicated 4KB Flash Sector at the very end of 2MB flash
inline constexpr uint32_t FLASH_CONFIG_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE; // 0x1FF000
inline constexpr uint32_t FLASH_CONFIG_ADDR   = XIP_BASE + FLASH_CONFIG_OFFSET;            // 0x101FF000
inline constexpr uint32_t FLASH_CONFIG_MAGIC  = 0x57414954; // ASCII "WAIT"
inline constexpr uint16_t FLASH_CONFIG_VERSION = 2;

struct __attribute__((packed)) FlashConfig {
    uint32_t magic;
    uint16_t version;
    char wifi_ssid[34];
    char wifi_password[64];
    char target_host[64];
    uint16_t target_port;
    char target_mac[18];
    char hostname[32];
    uint32_t wifi_auth;
    uint8_t connect_attempts;
    char motd[48];
    uint8_t reserved[3];
    uint32_t checksum;
};

inline void init_default_config(FlashConfig& cfg) {
    std::memset(&cfg, 0, sizeof(FlashConfig));
    cfg.magic = FLASH_CONFIG_MAGIC;
    cfg.version = FLASH_CONFIG_VERSION;
    cfg.wifi_ssid[0] = '\0';
    cfg.wifi_password[0] = '\0';
    std::strncpy(cfg.target_host, "64-pinned-potato-actual", sizeof(cfg.target_host) - 1);
    cfg.target_port = 25565;
    std::strncpy(cfg.target_mac, "AA:BB:CC:DD:EE:FF", sizeof(cfg.target_mac) - 1);
    std::strncpy(cfg.hostname, "WaitingServer", sizeof(cfg.hostname) - 1);
    cfg.wifi_auth = CYW43_AUTH_WPA2_MIXED_PSK;
    cfg.connect_attempts = 3;
    std::strncpy(cfg.motd, "❄ WaitingServer ✦ Pico W", sizeof(cfg.motd) - 1);
}

inline uint32_t compute_checksum(const FlashConfig& cfg) {
    const auto* data = reinterpret_cast<const uint8_t*>(&cfg);
    size_t len = offsetof(FlashConfig, checksum);
    uint32_t sum = 0x12345678;
    for (size_t i = 0; i < len; ++i) {
        sum = ((sum << 5) | (sum >> 27)) ^ data[i];
    }
    return sum;
}

inline bool load_flash_config(FlashConfig& out_cfg) {
    const auto* flash_ptr = reinterpret_cast<const FlashConfig*>(FLASH_CONFIG_ADDR);
    if (flash_ptr->magic != FLASH_CONFIG_MAGIC || flash_ptr->version != FLASH_CONFIG_VERSION) {
        return false;
    }
    if (compute_checksum(*flash_ptr) != flash_ptr->checksum) {
        return false;
    }
    std::memcpy(&out_cfg, flash_ptr, sizeof(FlashConfig));
    return true;
}

inline bool save_flash_config(const FlashConfig& cfg) {
    FlashConfig to_save = cfg;
    to_save.magic = FLASH_CONFIG_MAGIC;
    to_save.version = FLASH_CONFIG_VERSION;
    to_save.checksum = compute_checksum(to_save);

    alignas(4) uint8_t prog_buf[512] = {};
    std::memcpy(prog_buf, &to_save, sizeof(FlashConfig));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_CONFIG_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_CONFIG_OFFSET, prog_buf, sizeof(prog_buf));
    restore_interrupts(ints);

    const auto* verify = reinterpret_cast<const FlashConfig*>(FLASH_CONFIG_ADDR);
    bool ok = (verify->magic == FLASH_CONFIG_MAGIC &&
               verify->version == FLASH_CONFIG_VERSION &&
               verify->checksum == to_save.checksum);
    if (ok) {
        std::printf("[FLASH] Saved configuration (v%u) to sector 0x%06lX (SSID: '%s', Host: '%s', Hostname: '%s')\n",
                    to_save.version, static_cast<unsigned long>(FLASH_CONFIG_OFFSET),
                    to_save.wifi_ssid, to_save.target_host, to_save.hostname);
    } else {
        std::printf("[FLASH] Verification failed after programming flash!\n");
    }
    return ok;
}

} // namespace waiting_server
