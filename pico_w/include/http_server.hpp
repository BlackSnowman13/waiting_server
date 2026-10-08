#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "lwip/tcp.h"
#include "flash_config.hpp"
#include "portal_html.hpp"
#include "hardware/watchdog.h"
#include "pico/time.h"

namespace waiting_server {

class HttpServer {
public:
    struct tcp_pcb* pcb = nullptr;
    bool reboot_scheduled = false;
    uint32_t reboot_time_ms = 0;

    FlashConfig current_cfg = {};
    char failure_reason[160] = {};

    bool init(const FlashConfig& initial_cfg, const char* fail_reason = nullptr) {
        stop();
        current_cfg = initial_cfg;
        failure_reason[0] = '\0';
        if (fail_reason && fail_reason[0] != '\0') {
            std::strncpy(failure_reason, fail_reason, sizeof(failure_reason) - 1);
            failure_reason[sizeof(failure_reason) - 1] = '\0';
        }

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

        char first_line[64] = {};
        for (size_t i = 0; i < sizeof(first_line) - 1 && req_buf[i] != '\0' && req_buf[i] != '\r' && req_buf[i] != '\n'; ++i) {
            first_line[i] = req_buf[i];
        }
        std::printf("[HTTP] %s -> '%s'\n", ip4addr_ntoa(&tpcb->remote_ip), first_line);

        // 1. Check if saving configuration parameters
        if (std::strstr(req_buf, "POST /save") != nullptr || std::strstr(req_buf, "GET /save") != nullptr) {
            self->handle_save(tpcb, req_buf);
            return ERR_OK;
        }

        // 2. Ignore browser favicon requests
        if (std::strstr(req_buf, "GET /favicon.ico") != nullptr) {
            static constexpr const char FAVICON_RESP[] =
                "HTTP/1.1 204 No Content\r\n"
                "Connection: close\r\n\r\n";
            tcp_write(tpcb, FAVICON_RESP, sizeof(FAVICON_RESP) - 1, TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);
            tcp_close(tpcb);
            return ERR_OK;
        }

        // 3. Check if root setup form is requested
        bool is_root = (std::strncmp(req_buf, "GET / ", 6) == 0 ||
                        std::strncmp(req_buf, "GET /?", 6) == 0 ||
                        std::strncmp(req_buf, "GET /index.html", 15) == 0);

        // Check if Host header matches local server IP
        const char* host_hdr = std::strstr(req_buf, "Host:");
        bool is_ip_host = true;
        if (host_hdr) {
            host_hdr += 5;
            while (*host_hdr == ' ') host_hdr++;
            if (std::strncmp(host_hdr, "192.168.4.1", 11) != 0) {
                is_ip_host = false;
            }
        }

        if (is_root && is_ip_host) {
            self->handle_get_form(tpcb);
        } else {
            // Redirect captive portal detection probes (Android /generate_204,
            // iOS /hotspot-detect.html, Windows /connecttest.txt, or foreign hosts)
            // to http://192.168.4.1/
            self->handle_redirect(tpcb);
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

        val = find_param(req_buf, "hostname");
        if (val) url_decode(new_cfg.hostname, val, sizeof(new_cfg.hostname));

        val = find_param(req_buf, "motd");
        if (val) url_decode(new_cfg.motd, val, sizeof(new_cfg.motd));

        val = find_param(req_buf, "auth");
        if (val) {
            char auth_str[16] = {};
            url_decode(auth_str, val, sizeof(auth_str));
            new_cfg.wifi_auth = static_cast<uint32_t>(std::strtoul(auth_str, nullptr, 10));
        }

        val = find_param(req_buf, "attempts");
        if (val) {
            char att_str[10] = {};
            url_decode(att_str, val, sizeof(att_str));
            int att = std::atoi(att_str);
            if (att >= 1 && att <= 5) new_cfg.connect_attempts = static_cast<uint8_t>(att);
        }

        if (new_cfg.wifi_ssid[0] != '\0') {
            save_flash_config(new_cfg);
            current_cfg = new_cfg;
            std::printf("[CONFIG] Web setup updated config: SSID='%s', Host='%s:%d', MAC='%s', Hostname='%s', MOTD='%s'\n",
                        new_cfg.wifi_ssid, new_cfg.target_host, new_cfg.target_port,
                        new_cfg.target_mac, new_cfg.hostname, new_cfg.motd);
        } else {
            std::printf("[CONFIG] Web setup rejected empty SSID!\n");
        }

        tcp_write(tpcb, PORTAL_SAVE_SUCCESS_RESPONSE, sizeof(PORTAL_SAVE_SUCCESS_RESPONSE) - 1, TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
        tcp_close(tpcb);

        reboot_scheduled = true;
        reboot_time_ms = to_ms_since_boot(get_absolute_time()) + 1500;
    }

    void handle_redirect(struct tcp_pcb* tpcb) {
        std::printf("[HTTP] Redirecting %s (302 Found -> http://192.168.4.1/)\n",
                    ip4addr_ntoa(&tpcb->remote_ip));
        static constexpr const char REDIRECT_RESP[] =
            "HTTP/1.1 302 Found\r\n"
            "Location: http://192.168.4.1/\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        tcp_write(tpcb, REDIRECT_RESP, sizeof(REDIRECT_RESP) - 1, TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
        tcp_close(tpcb);
    }

    void handle_get_form(struct tcp_pcb* tpcb) {
        // Static buffer to eliminate stack consumption
        static char s_page_buf[6144];

        // Format error modal snippet if previous failure occurred
        char error_modal_buf[512] = {};
        if (failure_reason[0] != '\0') {
            std::snprintf(error_modal_buf, sizeof(error_modal_buf),
                          PORTAL_ERROR_MODAL_TEMPLATE, failure_reason);
        }

        // Format auth options dropdown
        char auth_options_buf[384] = {};
        std::snprintf(auth_options_buf, sizeof(auth_options_buf),
            "<option value='%lu'%s>WPA2 Mixed PSK (Default)</option>"
            "<option value='%lu'%s>WPA2 AES</option>"
            "<option value='%lu'%s>WPA TKIP</option>"
            "<option value='0'%s>Open (No Password)</option>",
            static_cast<unsigned long>(CYW43_AUTH_WPA2_MIXED_PSK),
            (current_cfg.wifi_auth == CYW43_AUTH_WPA2_MIXED_PSK) ? " selected" : "",
            static_cast<unsigned long>(CYW43_AUTH_WPA2_AES_PSK),
            (current_cfg.wifi_auth == CYW43_AUTH_WPA2_AES_PSK) ? " selected" : "",
            static_cast<unsigned long>(CYW43_AUTH_WPA_TKIP_PSK),
            (current_cfg.wifi_auth == CYW43_AUTH_WPA_TKIP_PSK) ? " selected" : "",
            (current_cfg.wifi_auth == CYW43_AUTH_OPEN) ? " selected" : ""
        );

        int len = std::snprintf(s_page_buf, sizeof(s_page_buf),
            PORTAL_HTML_TEMPLATE,
            error_modal_buf,
            current_cfg.wifi_ssid,
            current_cfg.wifi_password,
            auth_options_buf,
            static_cast<unsigned>(current_cfg.connect_attempts),
            current_cfg.hostname,
            current_cfg.motd,
            current_cfg.target_host,
            current_cfg.target_mac,
            static_cast<unsigned>(current_cfg.target_port)
        );

        if (len > 0) {
            size_t actual_body_len = std::min(static_cast<size_t>(len), sizeof(s_page_buf) - 1);
            char header_buf[160];
            int hdr_len = std::snprintf(header_buf, sizeof(header_buf),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n",
                actual_body_len
            );

            std::printf("[HTTP] Serving setup page (%zu bytes) to %s\n",
                        actual_body_len, ip4addr_ntoa(&tpcb->remote_ip));

            err_t err1 = tcp_write(tpcb, header_buf, static_cast<u16_t>(hdr_len), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
            err_t err2 = tcp_write(tpcb, s_page_buf, static_cast<u16_t>(actual_body_len), TCP_WRITE_FLAG_COPY);
            if (err1 != ERR_OK || err2 != ERR_OK) {
                std::printf("[HTTP] tcp_write failed: h_err=%d, b_err=%d\n", err1, err2);
            }
            tcp_output(tpcb);
        }
        tcp_close(tpcb);
    }
};

} // namespace waiting_server
