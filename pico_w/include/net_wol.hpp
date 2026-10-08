#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

namespace waiting_server {

// Parses MAC address string formatted as "AA:BB:CC:DD:EE:FF" or "AA-BB-CC-DD-EE-FF"
inline bool parse_mac_address(const char* mac_str, uint8_t mac[6]) {
    if (!mac_str) return false;
    unsigned int bytes[6];
    if (std::sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                    &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) == 6 ||
        std::sscanf(mac_str, "%x-%x-%x-%x-%x-%x",
                    &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            mac[i] = static_cast<uint8_t>(bytes[i]);
        }
        return true;
    }
    return false;
}

// Broadcasts a 102-byte Wake-on-LAN magic packet over UDP (port 9)
inline bool send_wake_on_lan(const uint8_t mac[6]) {
    uint8_t packet[102];
    std::memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(packet + 6 + (i * 6), mac, 6);
    }

    struct udp_pcb* pcb = udp_new();
    if (!pcb) {
        std::printf("[WOL] Failed to allocate UDP PCB\n");
        return false;
    }

    // Enable broadcast transmission
    ip_set_option(pcb, SOF_BROADCAST);

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, sizeof(packet), PBUF_RAM);
    if (!p) {
        std::printf("[WOL] Failed to allocate pbuf\n");
        udp_remove(pcb);
        return false;
    }

    std::memcpy(p->payload, packet, sizeof(packet));

    ip_addr_t broadcast_addr;
    ip_addr_set_ip4_u32(&broadcast_addr, IPADDR_BROADCAST);

    err_t err = udp_sendto(pcb, p, &broadcast_addr, 9);
    pbuf_free(p);
    udp_remove(pcb);

    if (err == ERR_OK) {
        std::printf("[WOL] Broadcasted magic packet for %02X:%02X:%02X:%02X:%02X:%02X to 255.255.255.255:9\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return true;
    } else {
        std::printf("[WOL] udp_sendto failed with error code %d\n", static_cast<int>(err));
        return false;
    }
}

inline bool send_wake_on_lan(const char* mac_str) {
    uint8_t mac[6];
    if (parse_mac_address(mac_str, mac)) {
        return send_wake_on_lan(mac);
    }
    std::printf("[WOL] Invalid MAC address format: %s\n", mac_str ? mac_str : "null");
    return false;
}

} // namespace waiting_server
