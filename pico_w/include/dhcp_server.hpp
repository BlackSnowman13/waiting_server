#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

namespace waiting_server {

class DhcpServer {
public:
    struct udp_pcb* pcb = nullptr;
    ip4_addr_t server_ip = {};
    ip4_addr_t client_ip = {};
    ip4_addr_t netmask   = {};

    bool init(ip4_addr_t s_ip, ip4_addr_t c_ip, ip4_addr_t mask) {
        stop();
        server_ip = s_ip;
        client_ip = c_ip;
        netmask   = mask;

        pcb = udp_new();
        if (!pcb) {
            std::printf("[DHCP] Failed to allocate UDP PCB\n");
            return false;
        }

        ip_set_option(pcb, SOF_BROADCAST);

        err_t err = udp_bind(pcb, IP_ADDR_ANY, 67);
        if (err != ERR_OK) {
            std::printf("[DHCP] Failed to bind to port 67: %d\n", err);
            udp_remove(pcb);
            pcb = nullptr;
            return false;
        }

        udp_recv(pcb, &DhcpServer::on_recv, this);
        std::printf("[DHCP] Server listening on UDP port 67 (Leasing to %s)\n", ip4addr_ntoa(&client_ip));
        return true;
    }

    void stop() {
        if (pcb) {
            udp_remove(pcb);
            pcb = nullptr;
        }
    }

private:
    struct __attribute__((packed)) DhcpHeader {
        uint8_t  op;
        uint8_t  htype;
        uint8_t  hlen;
        uint8_t  hops;
        uint32_t xid;
        uint16_t secs;
        uint16_t flags;
        uint32_t ciaddr;
        uint32_t yiaddr;
        uint32_t siaddr;
        uint32_t giaddr;
        uint8_t  chaddr[16];
        uint8_t  sname[64];
        uint8_t  file[128];
        uint32_t magic;
    };

    static void on_recv(void* arg, struct udp_pcb* upcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
        (void)addr;
        (void)port;
        auto* self = static_cast<DhcpServer*>(arg);
        if (!self || !p || p->tot_len < sizeof(DhcpHeader)) {
            if (p) pbuf_free(p);
            return;
        }

        DhcpHeader req;
        pbuf_copy_partial(p, &req, sizeof(DhcpHeader), 0);

        // Check magic cookie: 0x63825363 (network order: 0x63, 0x82, 0x53, 0x63)
        if (ntohl(req.magic) != 0x63825363) {
            pbuf_free(p);
            return;
        }

        // Find DHCP Message Type (Option 53)
        uint8_t msg_type = 0;
        size_t opt_offset = sizeof(DhcpHeader);
        while (opt_offset < p->tot_len) {
            uint8_t opt_code = 0;
            pbuf_copy_partial(p, &opt_code, 1, opt_offset++);
            if (opt_code == 255) break; // End option
            if (opt_code == 0) continue; // Pad

            uint8_t opt_len = 0;
            if (opt_offset >= p->tot_len) break;
            pbuf_copy_partial(p, &opt_len, 1, opt_offset++);

            if (opt_code == 53 && opt_len >= 1) {
                pbuf_copy_partial(p, &msg_type, 1, opt_offset);
                break;
            }
            opt_offset += opt_len;
        }

        pbuf_free(p);

        if (msg_type == 1) { // DHCPDISCOVER -> Reply with DHCPOFFER (2)
            self->send_reply(upcb, req, 2);
        } else if (msg_type == 3) { // DHCPREQUEST -> Reply with DHCPACK (5)
            self->send_reply(upcb, req, 5);
        }
    }

    void send_reply(struct udp_pcb* upcb, const DhcpHeader& req, uint8_t reply_type) {
        uint8_t buf[512] = {};
        auto* rep = reinterpret_cast<DhcpHeader*>(buf);

        rep->op = 2; // BOOTREPLY
        rep->htype = req.htype;
        rep->hlen = req.hlen;
        rep->hops = 0;
        rep->xid = req.xid;
        rep->flags = req.flags;
        rep->ciaddr = 0;
        rep->yiaddr = client_ip.addr;
        rep->siaddr = server_ip.addr;
        rep->giaddr = 0;
        std::memcpy(rep->chaddr, req.chaddr, 16);
        rep->magic = htonl(0x63825363);

        size_t offset = sizeof(DhcpHeader);

        // Option 53: Message Type
        buf[offset++] = 53;
        buf[offset++] = 1;
        buf[offset++] = reply_type;

        // Option 54: Server Identifier
        buf[offset++] = 54;
        buf[offset++] = 4;
        std::memcpy(&buf[offset], &server_ip.addr, 4);
        offset += 4;

        // Option 51: Lease Time (86400s)
        buf[offset++] = 51;
        buf[offset++] = 4;
        uint32_t lease = htonl(86400);
        std::memcpy(&buf[offset], &lease, 4);
        offset += 4;

        // Option 1: Subnet Mask
        buf[offset++] = 1;
        buf[offset++] = 4;
        std::memcpy(&buf[offset], &netmask.addr, 4);
        offset += 4;

        // Option 3: Router
        buf[offset++] = 3;
        buf[offset++] = 4;
        std::memcpy(&buf[offset], &server_ip.addr, 4);
        offset += 4;

        // Option 6: Domain Name Server (DNS)
        buf[offset++] = 6;
        buf[offset++] = 4;
        std::memcpy(&buf[offset], &server_ip.addr, 4);
        offset += 4;

        // Option 114: Captive Portal URL (RFC 8908 / RFC 8910)
        static constexpr const char CAPTIVE_URL[] = "http://192.168.4.1/";
        buf[offset++] = 114;
        buf[offset++] = sizeof(CAPTIVE_URL) - 1;
        std::memcpy(&buf[offset], CAPTIVE_URL, sizeof(CAPTIVE_URL) - 1);
        offset += sizeof(CAPTIVE_URL) - 1;

        // Option 160: Captive Portal URL (Legacy)
        buf[offset++] = 160;
        buf[offset++] = sizeof(CAPTIVE_URL) - 1;
        std::memcpy(&buf[offset], CAPTIVE_URL, sizeof(CAPTIVE_URL) - 1);
        offset += sizeof(CAPTIVE_URL) - 1;

        // Option 255: End
        buf[offset++] = 255;

        struct pbuf* p_out = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(offset), PBUF_RAM);
        if (!p_out) return;

        std::memcpy(p_out->payload, buf, offset);

        ip_addr_t bcast;
        ip_addr_set_ip4_u32(&bcast, IPADDR_BROADCAST);
        udp_sendto(upcb, p_out, &bcast, 68);
        pbuf_free(p_out);

        std::printf("[DHCP] Sent %s to client MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                    reply_type == 2 ? "DHCPOFFER" : "DHCPACK",
                    req.chaddr[0], req.chaddr[1], req.chaddr[2],
                    req.chaddr[3], req.chaddr[4], req.chaddr[5]);
    }
};

} // namespace waiting_server
