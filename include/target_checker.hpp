#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "pico/time.h"
#include "net_wol.hpp"

namespace waiting_server {

class TargetChecker {
public:
    char target_host[64] = {};
    uint16_t target_port = 25565;
    char target_mac[20] = {};

    bool is_online = false;
    bool probe_in_progress = false;
    uint32_t probe_start_ms = 0;
    uint32_t last_check_ms = 0;

    struct tcp_pcb* probe_pcb = nullptr;
    ip_addr_t target_ip = {};
    bool ip_resolved = false;

    void init(const char* host, uint16_t port, const char* mac) {
        if (host) {
            std::strncpy(target_host, host, sizeof(target_host) - 1);
        }
        target_port = port;
        if (mac) {
            std::strncpy(target_mac, mac, sizeof(target_mac) - 1);
        }

        // Check if host is already a dotted-quad IPv4 address
        if (ipaddr_aton(target_host, &target_ip)) {
            ip_resolved = true;
            std::printf("[CHECKER] Target IP configured statically: %s\n", ipaddr_ntoa(&target_ip));
        } else {
            ip_resolved = false;
            std::printf("[CHECKER] Target host is domain '%s', will resolve via DNS\n", target_host);
        }
    }

    void trigger_wol() {
        if (target_mac[0] != '\0') {
            std::printf("[CHECKER] Triggering Wake-on-LAN for %s\n", target_mac);
            send_wake_on_lan(target_mac);
        }
    }

    void trigger_immediate_check() {
        if (!probe_in_progress) {
            start_probe();
        }
    }

    void poll(uint32_t current_ms, bool has_waiting_clients) {
        // Timeout check: abort stuck probes after 2500ms
        if (probe_in_progress) {
            if (current_ms - probe_start_ms > 2500) {
                std::printf("[CHECKER] Connection probe timed out. Server is OFFLINE.\n");
                cleanup_probe(false);
            }
            return;
        }

        // Determine poll interval: poll every 3s if clients are waiting, else every 15s
        uint32_t interval = has_waiting_clients ? 3000 : 15000;
        if (last_check_ms == 0 || (current_ms - last_check_ms >= interval)) {
            start_probe();
        }
    }

private:
    void start_probe() {
        last_check_ms = to_ms_since_boot(get_absolute_time());
        probe_start_ms = last_check_ms;
        probe_in_progress = true;

        if (!ip_resolved) {
            // Initiate DNS lookup
            err_t err = dns_gethostbyname(target_host, &target_ip, &TargetChecker::dns_callback, this);
            if (err == ERR_OK) {
                // Resolved from DNS cache
                ip_resolved = true;
                connect_tcp();
            } else if (err == ERR_INPROGRESS) {
                // Resolution in progress; callback will trigger connect_tcp
                return;
            } else {
                std::printf("[CHECKER] DNS lookup failed immediately: err %d\n", static_cast<int>(err));
                probe_in_progress = false;
            }
            return;
        }

        connect_tcp();
    }

    void connect_tcp() {
        probe_pcb = tcp_new();
        if (!probe_pcb) {
            std::printf("[CHECKER] Failed to allocate TCP PCB for probe\n");
            probe_in_progress = false;
            return;
        }

        tcp_arg(probe_pcb, this);
        tcp_err(probe_pcb, &TargetChecker::tcp_err_callback);

        err_t err = tcp_connect(probe_pcb, &target_ip, target_port, &TargetChecker::tcp_connected_callback);
        if (err != ERR_OK) {
            std::printf("[CHECKER] tcp_connect failed with error %d\n", static_cast<int>(err));
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
            std::printf("[CHECKER] Target server state changed: %s\n",
                        is_online ? ">>> ONLINE <<<" : ">>> OFFLINE <<<");
        }
    }

    static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
        auto* self = static_cast<TargetChecker*>(callback_arg);
        if (!self) return;

        if (ipaddr) {
            self->target_ip = *ipaddr;
            self->ip_resolved = true;
            std::printf("[CHECKER] DNS resolved '%s' to %s\n", name, ipaddr_ntoa(ipaddr));
            if (self->probe_in_progress) {
                self->connect_tcp();
            }
        } else {
            std::printf("[CHECKER] DNS resolution failed for '%s'\n", name ? name : "unknown");
            self->cleanup_probe(false);
        }
    }

    static err_t tcp_connected_callback(void *arg, struct tcp_pcb *tpcb, err_t err) {
        auto* self = static_cast<TargetChecker*>(arg);
        if (self) {
            std::printf("[CHECKER] Successfully connected to %s:%d (ONLINE)\n",
                        ipaddr_ntoa(&self->target_ip), self->target_port);
            self->cleanup_probe(true);
        }
        // Abort the probe connection cleanly to free the PCB immediately
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    static void tcp_err_callback(void *arg, err_t err) {
        auto* self = static_cast<TargetChecker*>(arg);
        if (self) {
            // PCB is already freed by lwIP when err callback is invoked
            self->cleanup_probe(false);
        }
    }
};

} // namespace waiting_server
