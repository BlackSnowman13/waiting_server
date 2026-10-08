#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "lwip/tcp.h"
#include "flash_config.hpp"
#include "hardware/watchdog.h"
#include "pico/time.h"

namespace waiting_server {

class HttpServer {
public:
    struct tcp_pcb* pcb = nullptr;
    bool reboot_scheduled = false;
    uint32_t reboot_time_ms = 0;

    FlashConfig current_cfg = {};

    bool init(const FlashConfig& initial_cfg) {
        current_cfg = initial_cfg;

        pcb = tcp_new();
        if (!pcb) {
            std::printf("[HTTP] Failed to allocate TCP PCB\n");
            return false;
        }

        err_t err = tcp_bind(pcb, IP_ADDR_ANY, 80);
        if (err != ERR_OK) {
            std::printf("[HTTP] Failed to bind to port 80: %d\n", err);
            tcp_close(pcb);
            pcb = nullptr;
            return false;
        }

        struct tcp_pcb* listen_pcb = tcp_listen_with_backlog(pcb, 2);
        if (!listen_pcb) {
            std::printf("[HTTP] Failed to listen on port 80\n");
            tcp_close(pcb);
            pcb = nullptr;
            return false;
        }

        pcb = listen_pcb;
        tcp_arg(pcb, this);
        tcp_accept(pcb, &HttpServer::on_accept);

        std::printf("[HTTP] Setup Web Server listening on port 80\n");
        return true;
    }

    void stop() {
        if (pcb) {
            tcp_close(pcb);
            pcb = nullptr;
        }
    }

    void poll(uint32_t now_ms) {
        if (reboot_scheduled && now_ms >= reboot_time_ms) {
            std::printf("[HTTP] Rebooting device now to apply new configuration...\n");
            watchdog_reboot(0, 0, 0);
        }
    }

private:
    static err_t on_accept(void* arg, struct tcp_pcb* newpcb, err_t err) {
        if (err != ERR_OK || !newpcb) return ERR_VAL;
        auto* self = static_cast<HttpServer*>(arg);
        tcp_arg(newpcb, self);
        tcp_recv(newpcb, &HttpServer::on_recv);
        tcp_err(newpcb, &HttpServer::on_err);
        return ERR_OK;
    }

    static void on_err(void* arg, err_t err) {
        (void)arg;
        (void)err;
    }

    static err_t on_recv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
        auto* self = static_cast<HttpServer*>(arg);
        if (!self || !p || err != ERR_OK) {
            if (p) pbuf_free(p);
            if (tpcb) tcp_close(tpcb);
            return ERR_OK;
        }

        char req_buf[1024] = {};
        size_t len = pbuf_copy_partial(p, req_buf, sizeof(req_buf) - 1, 0);
        req_buf[len] = '\0';
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);

        // Check if request is saving parameters (POST or GET /save)
        if (std::strstr(req_buf, "POST /save") != nullptr || std::strstr(req_buf, "GET /save?") != nullptr) {
            self->handle_save(tpcb, req_buf);
        } else {
            self->handle_get_form(tpcb);
        }

        return ERR_OK;
    }

    static void url_decode(char* dst, const char* src, size_t max_len) {
        size_t d = 0;
        for (size_t s = 0; src[s] != '\0' && src[s] != '&' && src[s] != ' ' && src[s] != '\r' && src[s] != '\n' && d + 1 < max_len; ++s) {
            if (src[s] == '+') {
                dst[d++] = ' ';
            } else if (src[s] == '%' && src[s+1] != '\0' && src[s+2] != '\0') {
                char hex[3] = { src[s+1], src[s+2], '\0' };
                dst[d++] = static_cast<char>(std::strtol(hex, nullptr, 16));
                s += 2;
            } else {
                dst[d++] = src[s];
            }
        }
        dst[d] = '\0';
    }

    static const char* find_param(const char* buf, const char* param_name) {
        if (!buf || !param_name) return nullptr;
        size_t plen = std::strlen(param_name);
        const char* p = buf;
        while ((p = std::strstr(p, param_name)) != nullptr) {
            bool valid_delim = (p == buf || *(p - 1) == '?' || *(p - 1) == '&');
            if (valid_delim && *(p + plen) == '=') {
                return p + plen + 1;
            }
            p += plen;
        }
        return nullptr;
    }

    void handle_save(struct tcp_pcb* tpcb, const char* req_buf) {
        FlashConfig new_cfg = current_cfg;

        const char* val = find_param(req_buf, "ssid");
        if (val) url_decode(new_cfg.wifi_ssid, val, sizeof(new_cfg.wifi_ssid));

        val = find_param(req_buf, "pass");
        if (val) url_decode(new_cfg.wifi_password, val, sizeof(new_cfg.wifi_password));

        val = find_param(req_buf, "host");
        if (val) url_decode(new_cfg.target_host, val, sizeof(new_cfg.target_host));

        val = find_param(req_buf, "port");
        if (val) {
            char port_str[10] = {};
            url_decode(port_str, val, sizeof(port_str));
            int port = std::atoi(port_str);
            if (port > 0 && port <= 65535) new_cfg.target_port = static_cast<uint16_t>(port);
        }

        val = find_param(req_buf, "mac");
        if (val) url_decode(new_cfg.target_mac, val, sizeof(new_cfg.target_mac));

        if (new_cfg.wifi_ssid[0] != '\0') {
            save_flash_config(new_cfg);
            current_cfg = new_cfg;
            std::printf("[CONFIG] Web setup updated config: SSID='%s', Host='%s:%d', MAC='%s'\n",
                        new_cfg.wifi_ssid, new_cfg.target_host, new_cfg.target_port, new_cfg.target_mac);
        } else {
            std::printf("[CONFIG] Web setup rejected empty SSID!\n");
        }

        static constexpr const char SAVE_RESP[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;padding:40px;}"
            ".card{background:#1e1e1e;border-radius:12px;padding:30px;max-width:400px;margin:auto;box-shadow:0 4px 16px rgba(0,0,0,0.5);}"
            "h2{color:#4fc3f7;}p{color:#aaa;line-height:1.5;}</style></head><body>"
            "<div class='card'>"
            "<h2>&#10004; Settings Saved!</h2>"
            "<p>WaitingServer is restarting now and connecting to your Wi-Fi network.</p>"
            "<p>The LED will transition to a steady 1 Hz blink once connected.</p>"
            "</div></body></html>";

        tcp_write(tpcb, SAVE_RESP, sizeof(SAVE_RESP) - 1, TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
        tcp_close(tpcb);

        reboot_scheduled = true;
        reboot_time_ms = to_ms_since_boot(get_absolute_time()) + 1500;
    }

    void handle_get_form(struct tcp_pcb* tpcb) {
        char page_buf[2500];
        int len = std::snprintf(page_buf, sizeof(page_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>WaitingServer Setup</title>"
            "<style>"
            "*{box-sizing:border-box;}body{background:#121212;color:#eee;font-family:-apple-system,sans-serif;margin:0;padding:20px;display:flex;justify-content:center;}"
            ".card{background:#1e1e1e;border-radius:14px;padding:28px;width:100%%;max-width:420px;box-shadow:0 8px 24px rgba(0,0,0,0.6);border:1px solid #2a2a2a;}"
            "h2{margin-top:0;color:#38bdf8;font-size:22px;display:flex;align-items:center;gap:8px;}"
            "label{display:block;margin:14px 0 5px;font-size:13px;color:#94a3b8;font-weight:600;}"
            "input{width:100%%;padding:11px 13px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#fff;font-size:15px;outline:none;}"
            "input:focus{border-color:#38bdf8;}"
            "button{width:100%%;margin-top:22px;padding:13px;border-radius:8px;border:none;background:#0284c7;color:#fff;font-size:16px;font-weight:bold;cursor:pointer;}"
            "button:hover{background:#0369a1;}"
            ".note{font-size:12px;color:#64748b;margin-top:16px;text-align:center;}"
            "</style></head><body>"
            "<div class='card'>"
            "<h2>&#10052; WaitingServer Setup</h2>"
            "<form action='/save' method='GET'>"
            "<label>Wi-Fi Network (SSID)</label>"
            "<input type='text' name='ssid' value='%s' required>"
            "<label>Wi-Fi Password</label>"
            "<input type='password' name='pass' value='%s'>"
            "<label>Primary Server Host / IP</label>"
            "<input type='text' name='host' value='%s' required>"
            "<label>Primary Server Port</label>"
            "<input type='number' name='port' value='%u' required>"
            "<label>Target MAC (Wake-on-LAN)</label>"
            "<input type='text' name='mac' value='%s' required>"
            "<button type='submit'>Save & Connect</button>"
            "</form>"
            "<div class='note'>Pico W will reboot and connect to this network.</div>"
            "</div></body></html>",
            current_cfg.wifi_ssid,
            current_cfg.wifi_password,
            current_cfg.target_host,
            current_cfg.target_port,
            current_cfg.target_mac
        );

        if (len > 0) {
            tcp_write(tpcb, page_buf, static_cast<u16_t>(len), TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);
        }
        tcp_close(tpcb);
    }
};

} // namespace waiting_server
