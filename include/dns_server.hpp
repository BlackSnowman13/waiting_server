#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

namespace waiting_server {

class DnsServer {
public:
    struct udp_pcb* pcb = nullptr;
    ip4_addr_t redirect_ip = {};

    bool init(ip4_addr_t redir_ip) {
        redirect_ip = redir_ip;

        pcb = udp_new();
        if (!pcb) {
            std::printf("[DNS] Failed to allocate UDP PCB\n");
            return false;
        }

        err_t err = udp_bind(pcb, IP_ADDR_ANY, 53);
        if (err != ERR_OK) {
            std::printf("[DNS] Failed to bind to port 53: %d\n", err);
            udp_remove(pcb);
            pcb = nullptr;
            return false;
        }

        udp_recv(pcb, &DnsServer::on_recv, this);
        std::printf("[DNS] Captive Portal DNS server listening on UDP port 53 -> %s\n",
                    ip4addr_ntoa(&redirect_ip));
        return true;
    }

    void stop() {
        if (pcb) {
            udp_remove(pcb);
            pcb = nullptr;
        }
    }

private:
    static void on_recv(void* arg, struct udp_pcb* upcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
        auto* self = static_cast<DnsServer*>(arg);
        if (!self || !p || p->tot_len < 12) {
            if (p) pbuf_free(p);
            return;
        }

        uint8_t req[256];
        size_t req_len = pbuf_copy_partial(p, req, sizeof(req), 0);
        pbuf_free(p);

        if (req_len < 12) return;

        // Construct DNS Answer
        uint8_t resp[300];
        std::memcpy(resp, req, req_len);

        // Flags: Standard query response, No error, Authoritative
        resp[2] = 0x81;
        resp[3] = 0x80;
        // Answer Count = 1
        resp[6] = 0x00;
        resp[7] = 0x01;
        // Authority Count = 0
        resp[8] = 0x00;
        resp[9] = 0x00;
        // Additional Count = 0
        resp[10] = 0x00;
        resp[11] = 0x00;

        size_t ans_offset = req_len;
        if (ans_offset + 16 > sizeof(resp)) return;

        // Compression pointer to question name at offset 12 (0x0C)
        resp[ans_offset++] = 0xC0;
        resp[ans_offset++] = 0x0C;
        // Type: A (1)
        resp[ans_offset++] = 0x00;
        resp[ans_offset++] = 0x01;
        // Class: IN (1)
        resp[ans_offset++] = 0x00;
        resp[ans_offset++] = 0x01;
        // TTL: 60 seconds
        resp[ans_offset++] = 0x00;
        resp[ans_offset++] = 0x00;
        resp[ans_offset++] = 0x00;
        resp[ans_offset++] = 0x3C;
        // Data length: 4 bytes
        resp[ans_offset++] = 0x00;
        resp[ans_offset++] = 0x04;
        // IPv4 Address
        std::memcpy(&resp[ans_offset], &self->redirect_ip.addr, 4);
        ans_offset += 4;

        struct pbuf* p_out = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(ans_offset), PBUF_RAM);
        if (!p_out) return;

        std::memcpy(p_out->payload, resp, ans_offset);
        udp_sendto(upcb, p_out, addr, port);
        pbuf_free(p_out);
    }
};

} // namespace waiting_server
