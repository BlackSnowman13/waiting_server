#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "pico/time.h"
#include "net_wol.hpp"

namespace waiting_server {

class TargetChecker {
public:
    char target_host[64] = {};
    uint16_t target_port = 25565;
    char target_mac[20] = {};
    uint8_t target_mac_bytes[6] = {};
    bool has_valid_mac = false;

    bool is_online = false;
    bool probe_in_progress = false;
    uint32_t probe_start_ms = 0;
    uint32_t last_check_ms = 0;

    struct tcp_pcb* probe_pcb = nullptr;
    ip4_addr_t target_ip = {};
    char target_ip_str[16] = {};
    bool arp_resolved = false;

    // On-demand ARP scanning state
    bool scan_in_progress = false;
    uint32_t scan_cursor_u32 = 0;
    uint32_t scan_end_u32 = 0;
    uint32_t last_scan_step_ms = 0;
    uint8_t scan_round = 0;

    void init(const char* host, uint16_t port, const char* mac) {
        if (host) {
            std::strncpy(target_host, host, sizeof(target_host) - 1);
        }
        target_port = port;
        if (mac) {
            std::strncpy(target_mac, mac, sizeof(target_mac) - 1);
        }
        has_valid_mac = parse_mac(target_mac, target_mac_bytes);
        if (has_valid_mac) {
            std::printf("[CHECKER] Target MAC: %02X:%02X:%02X:%02X:%02X:%02X (ARP discovery enabled)\n",
                        target_mac_bytes[0], target_mac_bytes[1], target_mac_bytes[2],
                        target_mac_bytes[3], target_mac_bytes[4], target_mac_bytes[5]);
        } else {
            std::printf("[CHECKER] No valid target MAC provided: '%s'\n", target_mac);
        }

        // If target_host is a static IP and not "auto", initialize with it as fallback
        ip4_addr_t static_ip;
        if (target_host[0] != '\0' && std::strcmp(target_host, "auto") != 0 && ip4addr_aton(target_host, &static_ip)) {
            target_ip = static_ip;
            ip4addr_ntoa_r(&target_ip, target_ip_str, sizeof(target_ip_str));
            arp_resolved = true;
            std::printf("[CHECKER] Static target IP fallback: %s\n", target_ip_str);
        } else {
            arp_resolved = false;
            target_ip_str[0] = '\0';
        }
    }

    void trigger_wol() {
        if (has_valid_mac) {
            std::printf("[CHECKER] Triggering Wake-on-LAN for %s\n", target_mac);
            send_wake_on_lan(target_mac);
            start_arp_scan();
        }
    }

    void trigger_immediate_check() {
        if (arp_resolved && !probe_in_progress) {
            start_tcp_probe();
        } else if (!arp_resolved && has_valid_mac) {
            start_arp_scan();
        }
    }

    void start_arp_scan() {
        if (netif_default == nullptr) return;
        const ip4_addr_t* host_ip = netif_ip4_addr(netif_default);
        const ip4_addr_t* mask = netif_ip4_netmask(netif_default);
        if (!host_ip || !mask) return;

        uint32_t host_u32 = ntohl(ip4_addr_get_u32(host_ip));
        uint32_t mask_u32 = ntohl(ip4_addr_get_u32(mask));

        scan_cursor_u32 = (host_u32 & mask_u32) + 1;
        scan_end_u32    = (host_u32 | ~mask_u32) - 1;
        scan_in_progress = true;
        scan_round = 0;
        last_scan_step_ms = 0;

        char host_buf[16] = {};
        char mask_buf[16] = {};
        ip4addr_ntoa_r(host_ip, host_buf, sizeof(host_buf));
        ip4addr_ntoa_r(mask, mask_buf, sizeof(mask_buf));

        std::printf("[ARP] Starting on-demand ARP scan for MAC %s across %s/%s...\n",
                    target_mac, host_buf, mask_buf);

        // Check if MAC is already present in lwIP ARP cache
        if (check_arp_cache()) {
            scan_in_progress = false;
            start_tcp_probe();
        }
    }

    bool check_arp_cache() {
        if (!has_valid_mac) return false;
        ip4_addr_t *entry_ip = nullptr;
        struct netif *entry_netif = nullptr;
        struct eth_addr *entry_eth = nullptr;

        for (size_t i = 0; i < ARP_TABLE_SIZE; ++i) {
            if (etharp_get_entry(i, &entry_ip, &entry_netif, &entry_eth) && entry_ip && entry_eth) {
                if (std::memcmp(entry_eth->addr, target_mac_bytes, 6) == 0) {
                    target_ip = *entry_ip;
                    ip4addr_ntoa_r(&target_ip, target_ip_str, sizeof(target_ip_str));
                    arp_resolved = true;
                    std::printf("[ARP] Discovered Target IP %s for MAC %s!\n", target_ip_str, target_mac);
                    return true;
                }
            }
        }
        return false;
    }

    void poll(uint32_t current_ms, bool has_waiting_clients) {
        // 1. Progress on-demand ARP scanning
        if (scan_in_progress && !arp_resolved) {
            if (current_ms - last_scan_step_ms >= 50) {
                last_scan_step_ms = current_ms;

                if (check_arp_cache()) {
                    scan_in_progress = false;
                    start_tcp_probe();
                    return;
                }

                if (netif_default != nullptr) {
                    uint32_t my_ip = ntohl(ip4_addr_get_u32(netif_ip4_addr(netif_default)));
                    for (int k = 0; k < 16 && scan_cursor_u32 <= scan_end_u32; ++k, ++scan_cursor_u32) {
                        if (scan_cursor_u32 == my_ip) continue;
                        ip4_addr_t cand_ip;
                        ip4_addr_set_u32(&cand_ip, htonl(scan_cursor_u32));
                        etharp_request(netif_default, &cand_ip);
                    }

                    if (scan_cursor_u32 > scan_end_u32) {
                        scan_round++;
                        const ip4_addr_t* host_ip = netif_ip4_addr(netif_default);
                        const ip4_addr_t* mask = netif_ip4_netmask(netif_default);
                        uint32_t host_u32 = ntohl(ip4_addr_get_u32(host_ip));
                        uint32_t mask_u32 = ntohl(ip4_addr_get_u32(mask));
                        scan_cursor_u32 = (host_u32 & mask_u32) + 1;

                        if (scan_round >= 6 && !has_waiting_clients) {
                            scan_in_progress = false;
                            std::printf("[ARP] Scan timed out. Target server did not respond to ARP.\n");
                        }
                    }
                }
            }
        }

        // 2. Timeout check for TCP probe
        if (probe_in_progress) {
            if (current_ms - probe_start_ms > 2500) {
                if (probe_pcb) {
                    tcp_arg(probe_pcb, nullptr);
                    tcp_err(probe_pcb, nullptr);
                    tcp_abort(probe_pcb);
                    probe_pcb = nullptr;
                }
                cleanup_probe(false);
            }
            return;
        }

        // 3. Periodic probe
        if (arp_resolved) {
            uint32_t interval = has_waiting_clients ? 2000 : 15000;
            if (last_check_ms == 0 || (current_ms - last_check_ms >= interval)) {
                start_tcp_probe();
            }
        }
    }

private:
    static bool parse_mac(const char* str, uint8_t out[6]) {
        if (!str) return false;
        unsigned int b[6];
        if (std::sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 ||
            std::sscanf(str, "%x-%x-%x-%x-%x-%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(b[i]);
            return true;
        }
        return false;
    }

    void start_tcp_probe() {
        if (!arp_resolved) return;
        last_check_ms = to_ms_since_boot(get_absolute_time());
        probe_start_ms = last_check_ms;
        probe_in_progress = true;

        probe_pcb = tcp_new();
        if (!probe_pcb) {
            probe_in_progress = false;
            return;
        }

        tcp_arg(probe_pcb, this);
        tcp_err(probe_pcb, &TargetChecker::tcp_err_callback);

        ip_addr_t tip;
        ip_addr_copy_from_ip4(tip, target_ip);
        err_t err = tcp_connect(probe_pcb, &tip, target_port, &TargetChecker::tcp_connected_callback);
        if (err != ERR_OK) {
            tcp_arg(probe_pcb, nullptr);
            tcp_err(probe_pcb, nullptr);
            tcp_abort(probe_pcb);
            probe_pcb = nullptr;
            cleanup_probe(false);
        }
    }

    void cleanup_probe(bool success) {
        probe_in_progress = false;
        probe_pcb = nullptr;

        bool prev = is_online;
        is_online = success;
        if (prev != is_online) {
            std::printf("[CHECKER] Target server state changed: %s (%s:%u)\n",
                        is_online ? ">>> ONLINE <<<" : ">>> OFFLINE <<<",
                        target_ip_str, target_port);
        }
    }

    static err_t tcp_connected_callback(void *arg, struct tcp_pcb *tpcb, err_t err) {
        (void)err;
        auto* self = static_cast<TargetChecker*>(arg);
        if (self) {
            self->probe_pcb = nullptr;
            self->cleanup_probe(true);
        }
        // Detach callbacks so tcp_abort does NOT invoke tcp_err_callback
        tcp_arg(tpcb, nullptr);
        tcp_err(tpcb, nullptr);
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    static void tcp_err_callback(void *arg, err_t err) {
        (void)err;
        auto* self = static_cast<TargetChecker*>(arg);
        if (self) {
            self->probe_pcb = nullptr;
            self->cleanup_probe(false);
        }
    }
};

} // namespace waiting_server
