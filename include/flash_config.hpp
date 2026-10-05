#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "config.hpp"

namespace waiting_server {

// Dedicated 4KB Flash Sector at the very end of 2MB flash
inline constexpr uint32_t FLASH_CONFIG_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE; // 0x1FF000
inline constexpr uint32_t FLASH_CONFIG_ADDR   = XIP_BASE + FLASH_CONFIG_OFFSET;            // 0x101FF000
inline constexpr uint32_t FLASH_CONFIG_MAGIC  = 0x57414954; // ASCII "WAIT"

struct __attribute__((packed)) FlashConfig {
    uint32_t magic;
    uint16_t version;
    char wifi_ssid[64];
    char wifi_password[64];
    char target_host[64];
    uint16_t target_port;
    char target_mac[20];
    uint32_t checksum;
};

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
    if (flash_ptr->magic != FLASH_CONFIG_MAGIC || flash_ptr->version != 1) {
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
    to_save.version = 1;
    to_save.checksum = compute_checksum(to_save);

    alignas(4) uint8_t page_buf[FLASH_PAGE_SIZE] = {};
    std::memcpy(page_buf, &to_save, sizeof(FlashConfig));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_CONFIG_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_CONFIG_OFFSET, page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    const auto* verify = reinterpret_cast<const FlashConfig*>(FLASH_CONFIG_ADDR);
    bool ok = (verify->magic == FLASH_CONFIG_MAGIC && verify->checksum == to_save.checksum);
    if (ok) {
        std::printf("[FLASH] Saved configuration to sector 0x%06lX (SSID: '%s', Host: '%s')\n",
                    static_cast<unsigned long>(FLASH_CONFIG_OFFSET), to_save.wifi_ssid, to_save.target_host);
    } else {
        std::printf("[FLASH] Verification failed after programming flash!\n");
    }
    return ok;
}

} // namespace waiting_server
