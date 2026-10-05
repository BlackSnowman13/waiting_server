#include <cstdio>
#include <cstdint>
#include <cstring>
#include <array>
#include <cyw43_ll.h>
#include <span>
#include <string_view>
#include <algorithm>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "include/config.hpp"
#include "include/flash_config.hpp"
#include "include/dhcp_server.hpp"
#include "include/dns_server.hpp"
#include "include/http_server.hpp"
#include "include/packet_reader.hpp"
#include "include/packet_writer.hpp"
#include "include/server_status.hpp"
#include "include/config_packets.hpp"
#include "include/play_packets.hpp"
#include "include/target_checker.hpp"

// Global services
static waiting_server::TargetChecker g_target_checker;
static waiting_server::DhcpServer    g_dhcp_server;
static waiting_server::DnsServer     g_dns_server;
static waiting_server::HttpServer    g_http_server;

enum class RunMode {
    CaptivePortal, // Wi-Fi Setup Hotspot active
    ServerRunning  // Connected to Wi-Fi & Minecraft WaitingServer listening
};

static RunMode g_run_mode = RunMode::CaptivePortal;

enum class LedPattern {
    CaptivePortal, // on 200ms, off 200ms, on 200ms, off 1000ms (1600ms cycle)
    ServerRunning  // on 1000ms, off 1000ms (2000ms cycle)
};

// Non-blocking LED pattern generator
static void update_led(uint32_t now_ms, LedPattern pattern) {
    static bool current_state = false;
    bool new_state = false;

    if (pattern == LedPattern::CaptivePortal) {
        uint32_t phase = now_ms % 1600;
        if (phase < 200) {
            new_state = true;
        } else if (phase < 400) {
            new_state = false;
        } else if (phase < 600) {
            new_state = true;
        } else {
            new_state = false;
        }
    } else { // ServerRunning
        uint32_t phase = now_ms % 2000;
        new_state = (phase < 1000);
    }

    if (new_state != current_state) {
        current_state = new_state;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, new_state ? 1 : 0);
    }
}

// Forward declaration
struct ClientConnection;
static void close_client(ClientConnection* client);
static void pump_tx(ClientConnection* client);
static void advance_config_queue(ClientConnection* client);
static void advance_play_queue(ClientConnection* client);
static void send_play_transfer(ClientConnection* client);
static void send_play_keepalive(ClientConnection* client, uint32_t now);
static void send_play_action_bar(ClientConnection* client, std::string_view text);

// Static Client Connection representation (Zero heap allocation)
struct ClientConnection {
    tcp_pcb* pcb = nullptr;
    size_t index = 0;

    enum class State : uint8_t {
        Handshake,
        Status,
        Login,
        Configuration,
        Play,
        Closed
    } state = State::Handshake;

    std::array<std::byte, 1024> rx_buffer = {};
    size_t rx_len = 0;

    // Fixed pre-allocated TX buffer for dynamic responses
    std::array<std::byte, 1024> tx_buffer = {};

    char player_name[20] = {};
    std::array<std::byte, 16> player_uuid = {};

    // Current streaming slice
    const uint8_t* tx_stream_ptr = nullptr;
    size_t tx_stream_remaining = 0;
    bool tx_is_rom = false;

    // Progression in packet sequence
    size_t config_packet_index = 0;
    size_t play_packet_index = 0;
    bool sending_config_queue = false;
    bool sending_play_queue = false;

    uint32_t last_keepalive_sent_ms = 0;
    uint32_t last_actionbar_sent_ms = 0;

    bool is_active = false;
    bool close_after_sent = false;

    void reset() {
        pcb = nullptr;
        state = State::Handshake;
        rx_len = 0;
        player_name[0] = '\0';
        player_uuid.fill(std::byte{0});
        tx_stream_ptr = nullptr;
        tx_stream_remaining = 0;
        tx_is_rom = false;
        config_packet_index = 0;
        play_packet_index = 0;
        sending_config_queue = false;
        sending_play_queue = false;
        last_keepalive_sent_ms = 0;
        last_actionbar_sent_ms = 0;
        is_active = false;
        close_after_sent = false;
    }
};

static ClientConnection g_clients[waiting_server::MAX_CLIENTS];

// Allocate a slot from the static client pool
static ClientConnection* allocate_client(tcp_pcb* pcb) {
    for (size_t i = 0; i < waiting_server::MAX_CLIENTS; ++i) {
        if (!g_clients[i].is_active) {
            g_clients[i].reset();
            g_clients[i].pcb = pcb;
            g_clients[i].index = i;
            g_clients[i].is_active = true;
            return &g_clients[i];
        }
    }
    return nullptr;
}

// Safely close connection and release static slot
static void close_client(ClientConnection* client) {
    if (!client) return;
    if (client->pcb) {
        tcp_arg(client->pcb, nullptr);
        tcp_recv(client->pcb, nullptr);
        tcp_sent(client->pcb, nullptr);
        tcp_err(client->pcb, nullptr);
        tcp_poll(client->pcb, nullptr, 0);
        tcp_close(client->pcb);
        client->pcb = nullptr;
    }
    client->reset();
}

// Sends a single dynamic packet from RAM
static void send_dynamic_packet(ClientConnection* client, const void* data, size_t size, bool close_after = false) {
    if (!client || !client->pcb || size == 0) return;
    client->close_after_sent = close_after;
    err_t err = tcp_write(client->pcb, data, static_cast<u16_t>(size), TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        tcp_output(client->pcb);
    } else {
        std::printf("[ERROR] tcp_write dynamic packet failed: %d\n", err);
    }
}

// Advances to the next configuration packet in the sequence
static void advance_config_queue(ClientConnection* client) {
    if (!client || !client->sending_config_queue) return;

    if (client->config_packet_index < waiting_server::NUM_CONFIG_PACKETS) {
        auto blob = waiting_server::CONFIG_PACKETS[client->config_packet_index++];
        client->tx_stream_ptr = blob.data;
        client->tx_stream_remaining = blob.size;
        client->tx_is_rom = true;
    } else {
        client->sending_config_queue = false;
        client->tx_stream_ptr = nullptr;
        client->tx_stream_remaining = 0;
        std::printf("[CONFIG] Finished sending all configuration packets to '%s'. Awaiting FinishConfig ACK...\n",
                    client->player_name);
    }
}

// Advances to the next play packet in the sequence
static void advance_play_queue(ClientConnection* client) {
    if (!client || !client->sending_play_queue) return;

    if (client->play_packet_index < waiting_server::NUM_PLAY_PACKETS) {
        if (client->play_packet_index == 0) { // Login (Play) packet - ID 0x31 (Dynamic entity ID in bytes 2..5)
            int32_t entity_id = 300 + static_cast<int32_t>(client->index);
            std::memcpy(client->tx_buffer.data(), waiting_server::play_packet_000, sizeof(waiting_server::play_packet_000));
            client->tx_buffer[2] = static_cast<std::byte>((entity_id >> 24) & 0xFF);
            client->tx_buffer[3] = static_cast<std::byte>((entity_id >> 16) & 0xFF);
            client->tx_buffer[4] = static_cast<std::byte>((entity_id >> 8) & 0xFF);
            client->tx_buffer[5] = static_cast<std::byte>(entity_id & 0xFF);

            client->tx_stream_ptr = reinterpret_cast<const uint8_t*>(client->tx_buffer.data());
            client->tx_stream_remaining = sizeof(waiting_server::play_packet_000);
            client->tx_is_rom = false;
            client->play_packet_index++;
        } else if (client->play_packet_index == 5) { // Entity Event (Op Permission Level)
            int32_t entity_id = 300 + static_cast<int32_t>(client->index);
            std::memcpy(client->tx_buffer.data(), waiting_server::play_packet_005, sizeof(waiting_server::play_packet_005));
            client->tx_buffer[2] = static_cast<std::byte>((entity_id >> 24) & 0xFF);
            client->tx_buffer[3] = static_cast<std::byte>((entity_id >> 16) & 0xFF);
            client->tx_buffer[4] = static_cast<std::byte>((entity_id >> 8) & 0xFF);
            client->tx_buffer[5] = static_cast<std::byte>(entity_id & 0xFF);

            client->tx_stream_ptr = reinterpret_cast<const uint8_t*>(client->tx_buffer.data());
            client->tx_stream_remaining = sizeof(waiting_server::play_packet_005);
            client->tx_is_rom = false;
            client->play_packet_index++;
        } else if (client->play_packet_index == 9) { // Synchronize Player Position (ID 0x48)
            std::memcpy(client->tx_buffer.data(), waiting_server::play_packet_009, sizeof(waiting_server::play_packet_009));
            auto write_be_double = [](std::byte* dst, double val) {
                uint64_t u;
                std::memcpy(&u, &val, sizeof(u));
                #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                u = __builtin_bswap64(u);
                #endif
                std::memcpy(dst, &u, sizeof(u));
            };
            write_be_double(&client->tx_buffer[3], waiting_server::LOBBY_SPAWN_X);
            write_be_double(&client->tx_buffer[11], waiting_server::LOBBY_SPAWN_Y);
            write_be_double(&client->tx_buffer[19], waiting_server::LOBBY_SPAWN_Z);

            client->tx_stream_ptr = reinterpret_cast<const uint8_t*>(client->tx_buffer.data());
            client->tx_stream_remaining = sizeof(waiting_server::play_packet_009);
            client->tx_is_rom = false;
            client->play_packet_index++;
        } else if (client->play_packet_index == 12) { // Player Info Update (ID 0x46)
            waiting_server::PacketWriter writer(client->tx_buffer);
            if (writer.write_varint(0x46) &&
                writer.write_byte(std::byte{0xFF}) && // action mask (all actions)
                writer.write_varint(1) &&              // number of entries
                writer.write_uuid(client->player_uuid) &&
                writer.write_string(std::string_view(client->player_name)) &&
                writer.write_varint(0) &&              // properties count (0)
                writer.write_bool(false) &&            // chat session
                writer.write_varint(0) &&              // gameMode (0 = survival)
                writer.write_bool(true) &&             // listed
                writer.write_varint(0) &&              // latency
                writer.write_bool(false) &&            // displayName
                writer.write_varint(0) &&              // listOrder
                writer.write_bool(false)) {            // showHat

                auto resp = writer.finalize();
                if (resp) {
                    client->tx_stream_ptr = reinterpret_cast<const uint8_t*>(resp->data());
                    client->tx_stream_remaining = resp->size();
                    client->tx_is_rom = false;
                    client->play_packet_index++;
                } else {
                    close_client(client);
                }
            } else {
                close_client(client);
            }
        } else if (client->play_packet_index == 19) { // Set Chunk Cache Center (ID 0x5E)
            waiting_server::PacketWriter writer(client->tx_buffer);
            if (writer.write_varint(0x5E) &&
                writer.write_varint(waiting_server::LOBBY_CENTER_CHUNK_X) &&
                writer.write_varint(waiting_server::LOBBY_CENTER_CHUNK_Z)) {
                auto resp = writer.finalize();
                if (resp) {
                    client->tx_stream_ptr = reinterpret_cast<const uint8_t*>(resp->data());
                    client->tx_stream_remaining = resp->size();
                    client->tx_is_rom = false;
                    client->play_packet_index++;
                } else {
                    close_client(client);
                }
            } else {
                close_client(client);
            }
        } else {
            auto blob = waiting_server::PLAY_PACKETS[client->play_packet_index++];
            client->tx_stream_ptr = blob.data;
            client->tx_stream_remaining = blob.size;
            client->tx_is_rom = true;
        }
    } else if (client->play_packet_index == waiting_server::NUM_PLAY_PACKETS) {
        // Send Entity Metadata (0x63) to show player skin layers and finalize spawn
        waiting_server::PacketWriter writer(client->tx_buffer);
        int32_t entity_id = 300 + static_cast<int32_t>(client->index);
        if (writer.write_varint(0x63) &&
            writer.write_varint(entity_id) &&
            writer.write_byte(std::byte{16}) && // Index 16: skin display mask
            writer.write_varint(0) &&            // Type 0: Byte
            writer.write_byte(std::byte{0x7F}) && // Value: all layers enabled
            writer.write_byte(std::byte{0xFF})) { // 0xFF: End of metadata

            auto resp = writer.finalize();
            if (resp) {
                client->tx_stream_ptr = reinterpret_cast<const uint8_t*>(resp->data());
                client->tx_stream_remaining = resp->size();
                client->tx_is_rom = false;
                client->play_packet_index++;
            } else {
                close_client(client);
            }
        } else {
            close_client(client);
        }
    } else {
        client->sending_play_queue = false;
        client->tx_stream_ptr = nullptr;
        client->tx_stream_remaining = 0;
        std::printf("[PLAY] Player '%s' spawned in lobby world! (X=%.1f, Y=%.1f, Z=%.1f)\n",
                    client->player_name, waiting_server::SPAWN_X, waiting_server::SPAWN_Y, waiting_server::SPAWN_Z);

        if (g_target_checker.is_online) {
            send_play_transfer(client);
        } else {
            g_target_checker.trigger_wol();
        }
    }
}

// Pumps outbound data slices to lwIP TCP send buffer
static void pump_tx(ClientConnection* client) {
    if (!client || !client->pcb) return;

    while (true) {
        if (client->tx_stream_remaining == 0) {
            if (client->sending_config_queue) {
                advance_config_queue(client);
            } else if (client->sending_play_queue) {
                advance_play_queue(client);
            } else {
                break;
            }
        }

        if (client->tx_stream_remaining == 0) {
            break;
        }

        u16_t available = tcp_sndbuf(client->pcb);
        if (available == 0) {
            break; // Send buffer full; wait for on_tcp_sent callback
        }

        size_t to_write = std::min(static_cast<size_t>(available), client->tx_stream_remaining);
        uint8_t flags = client->tx_is_rom ? 0 : TCP_WRITE_FLAG_COPY;
        if (client->tx_stream_remaining > to_write || client->sending_config_queue || client->sending_play_queue) {
            flags |= TCP_WRITE_FLAG_MORE;
        }

        err_t err = tcp_write(client->pcb, client->tx_stream_ptr, static_cast<u16_t>(to_write), flags);
        if (err != ERR_OK) {
            break;
        }

        client->tx_stream_ptr += to_write;
        client->tx_stream_remaining -= to_write;
    }

    tcp_output(client->pcb);
}

// Sends a Play KeepAlive packet (0x2C)
static void send_play_keepalive(ClientConnection* client, uint32_t now) {
    if (!client || !client->pcb || client->sending_play_queue) return;
    uint64_t keepalive_id = static_cast<uint64_t>(now);
    waiting_server::PacketWriter writer(client->tx_buffer);
    if (writer.write_varint(0x2C) && writer.write_ulong(keepalive_id)) {
        auto resp = writer.finalize();
        if (resp) {
            send_dynamic_packet(client, resp->data(), resp->size(), false);
        }
    }
}

// Sends an Action Bar message to the player (Packet ID 0x57 in Protocol 776)
static void send_play_action_bar(ClientConnection* client, std::string_view text) {
    if (!client || !client->pcb || client->sending_play_queue) return;
    waiting_server::PacketWriter writer(client->tx_buffer);
    if (writer.write_varint(0x57) &&
        writer.write_byte(std::byte{0x08}) &&
        writer.write_ushort(static_cast<uint16_t>(text.size()))) {
        bool ok = true;
        for (char c : text) {
            if (!writer.write_byte(static_cast<std::byte>(c))) {
                ok = false;
                break;
            }
        }
        if (ok) {
            auto resp = writer.finalize();
            if (resp) {
                send_dynamic_packet(client, resp->data(), resp->size(), false);
            }
        }
    }
}

// Sends the Play Transfer packet (0x81) to redirect player to primary server
static void send_play_transfer(ClientConnection* client) {
    if (!client || !client->pcb || client->sending_play_queue) return;
    std::printf("[TRANSFER] Primary server ONLINE! Transferring '%s' to %s:%d\n",
                client->player_name, g_target_checker.target_host, g_target_checker.target_port);
    waiting_server::PacketWriter writer(client->tx_buffer);
    if (writer.write_varint(0x81) &&
        writer.write_string(g_target_checker.target_host) &&
        writer.write_varint(g_target_checker.target_port)) {
        auto resp = writer.finalize();
        if (resp) {
            send_dynamic_packet(client, resp->data(), resp->size(), true);
        }
    }
}

// Handle an individual decoded Minecraft packet according to protocol state
static bool handle_client_packet(ClientConnection* client, int32_t packet_id, std::span<const std::byte> payload) {
    switch (client->state) {
        case ClientConnection::State::Handshake: {
            if (packet_id == 0x00) { // Handshake Packet
                auto proto_opt = waiting_server::read_varint(payload);
                auto host_opt  = waiting_server::read_string(payload);
                auto port_opt  = waiting_server::read_ushort(payload);
                auto next_opt  = waiting_server::read_varint(payload);

                if (!proto_opt || !host_opt || !port_opt || !next_opt) {
                    std::printf("[ERROR] Malformed Handshake packet\n");
                    return false;
                }

                int32_t next_state = *next_opt;
                if (next_state == 1) {
                    client->state = ClientConnection::State::Status;
                    std::printf("[STATUS] Handshake -> State 1 (Status Ping), proto: %ld\n", static_cast<long>(*proto_opt));
                    return true;
                } else if (next_state == 2) {
                    client->state = ClientConnection::State::Login;
                    std::printf("[LOGIN] Handshake -> State 2 (Login Attempt), proto: %ld\n", static_cast<long>(*proto_opt));
                    return true;
                }
            }
            return false;
        }

        case ClientConnection::State::Status: {
            if (packet_id == 0x00) { // Status Request
                std::printf("[STATUS] Sending Status Response with Favicon (%s)...\n",
                            g_target_checker.is_online ? "Online" : "Sleeping");

                if (g_target_checker.is_online) {
                    client->tx_stream_ptr = waiting_server::STATUS_PACKET_ONLINE;
                    client->tx_stream_remaining = waiting_server::STATUS_PACKET_ONLINE_SIZE;
                } else {
                    client->tx_stream_ptr = waiting_server::STATUS_PACKET_OFFLINE;
                    client->tx_stream_remaining = waiting_server::STATUS_PACKET_OFFLINE_SIZE;
                }

                client->tx_is_rom = true;
                client->close_after_sent = false;
                pump_tx(client);
                return true;

            } else if (packet_id == 0x01) { // Ping Request
                auto payload_opt = waiting_server::read_ulong(payload);
                if (!payload_opt) return false;

                uint64_t ping_val = *payload_opt;
                waiting_server::PacketWriter writer(client->tx_buffer);
                if (!writer.write_varint(0x01)) return false; // Packet ID 0x01 (Pong)
                if (!writer.write_ulong(ping_val)) return false;

                auto resp = writer.finalize();
                if (!resp) return false;

                send_dynamic_packet(client, resp->data(), resp->size(), true);
                return true;
            }
            return false;
        }

        case ClientConnection::State::Login: {
            if (packet_id == 0x00) { // Login Start
                auto name_opt = waiting_server::read_string(payload);
                auto uuid_opt = waiting_server::read_uuid(payload);
                if (!name_opt || !uuid_opt) return false;

                std::string_view name = *name_opt;
                size_t copy_len = std::min(name.size(), sizeof(client->player_name) - 1);
                std::memcpy(client->player_name, name.data(), copy_len);
                client->player_name[copy_len] = '\0';
                client->player_uuid = *uuid_opt;

                std::printf("[LOGIN] Player '%s' is joining...\n", client->player_name);

                // Send Login Success (Packet ID 0x02)
                waiting_server::PacketWriter writer(client->tx_buffer);
                if (!writer.write_varint(0x02)) return false;
                if (!writer.write_uuid(client->player_uuid)) return false;
                if (!writer.write_string(std::string_view(client->player_name))) return false;
                if (!writer.write_varint(0)) return false; // 0 properties
                std::array<std::byte, 16> dummy_session_id = {};
                if (!writer.write_uuid(dummy_session_id)) return false;

                auto resp = writer.finalize();
                if (!resp) return false;

                send_dynamic_packet(client, resp->data(), resp->size(), false);
                return true;

            } else if (packet_id == 0x03) { // Login Acknowledged
                std::printf("[LOGIN] Login Acknowledged by '%s'. Starting Configuration sequence...\n", client->player_name);
                client->state = ClientConnection::State::Configuration;
                client->config_packet_index = 0;
                client->sending_config_queue = true;

                // Wake primary server if offline
                if (!g_target_checker.is_online) {
                    g_target_checker.trigger_wol();
                }
                g_target_checker.trigger_immediate_check();

                pump_tx(client);
                return true;
            }
            return false;
        }

        case ClientConnection::State::Configuration: {
            if (packet_id == 0x00) { // Client Information
                return true;
            } else if (packet_id == 0x07) { // Known Packs ACK
                return true;
            } else if (packet_id == 0x04) { // KeepAlive response
                return true;
            } else if (packet_id == 0x03) { // Finish Configuration ACK
                std::printf("[CONFIG] Finish Configuration ACK from '%s'! Transitioning to PLAY!\n", client->player_name);
                client->state = ClientConnection::State::Play;
                client->play_packet_index = 0;
                client->sending_play_queue = true;
                pump_tx(client);
                return true;
            }
            return true;
        }

        case ClientConnection::State::Play: {
            if (packet_id == 0x00) { // Teleport Confirm
                return true;
            } else if (packet_id == 0x0C || packet_id == 0x08) { // Chunk Batch Received
                return true;
            } else if (packet_id == 0x1C) { // KeepAlive response
                return true;
            }
            // Allow player position / movement packets
            return true;
        }

        default:
            return false;
    }
}

// lwIP Callback: Sent data acknowledged by client
static err_t on_tcp_sent(void* arg, struct tcp_pcb* tpcb, u16_t len) {
    (void)len;
    (void)tpcb;
    auto* client = static_cast<ClientConnection*>(arg);
    if (!client) return ERR_OK;

    pump_tx(client);

    if (client->tx_stream_remaining == 0 && !client->sending_config_queue && !client->sending_play_queue) {
        if (client->close_after_sent) {
            close_client(client);
        }
    }
    return ERR_OK;
}

// lwIP Callback: Incoming TCP data
static err_t on_tcp_recv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
    auto* client = static_cast<ClientConnection*>(arg);
    if (!client) {
        if (p) pbuf_free(p);
        return ERR_VAL;
    }

    if (p == nullptr) {
        std::printf("[INFO] Client '%s' disconnected\n", client->player_name[0] ? client->player_name : "unknown");
        close_client(client);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        close_client(client);
        return err;
    }

    if (client->rx_len + p->tot_len > client->rx_buffer.size()) {
        std::printf("[ERROR] RX buffer overflow! Dropping client.\n");
        pbuf_free(p);
        close_client(client);
        return ERR_MEM;
    }

    pbuf_copy_partial(p, client->rx_buffer.data() + client->rx_len, p->tot_len, 0);
    client->rx_len += p->tot_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // Process all framed Minecraft packets in rx_buffer
    while (client->rx_len > 0) {
        std::span<const std::byte> rx_span(client->rx_buffer.data(), client->rx_len);
        auto pkt_len_opt = waiting_server::read_varint(rx_span);
        if (!pkt_len_opt) {
            break;
        }

        int32_t packet_length = *pkt_len_opt;
        if (packet_length <= 0 || packet_length > 2048) {
            std::printf("[ERROR] Invalid packet length: %ld\n", static_cast<long>(packet_length));
            close_client(client);
            return ERR_OK;
        }

        size_t header_len = client->rx_len - rx_span.size();
        if (rx_span.size() < static_cast<size_t>(packet_length)) {
            break;
        }

        std::span<const std::byte> payload_span = rx_span.subspan(0, static_cast<size_t>(packet_length));
        auto pkt_id_opt = waiting_server::read_varint(payload_span);
        if (!pkt_id_opt) {
            close_client(client);
            return ERR_OK;
        }

        int32_t packet_id = *pkt_id_opt;
        bool keep_open = handle_client_packet(client, packet_id, payload_span);

        size_t total_consumed = header_len + static_cast<size_t>(packet_length);
        size_t remaining = client->rx_len - total_consumed;
        if (remaining > 0) {
            std::memmove(client->rx_buffer.data(), client->rx_buffer.data() + total_consumed, remaining);
        }
        client->rx_len = remaining;

        if (!keep_open) {
            close_client(client);
            break;
        }
    }

    return ERR_OK;
}

// lwIP Callback: TCP connection error / reset
static void on_tcp_err(void* arg, err_t err) {
    (void)err;
    auto* client = static_cast<ClientConnection*>(arg);
    if (client) {
        client->pcb = nullptr; // lwIP already deallocated PCB
        client->reset();
    }
}

// lwIP Callback: Periodic poll timer
static err_t on_tcp_poll(void* arg, struct tcp_pcb* tpcb) {
    (void)tpcb;
    auto* client = static_cast<ClientConnection*>(arg);
    if (client && (client->sending_config_queue || client->sending_play_queue)) {
        pump_tx(client);
    }
    return ERR_OK;
}

// lwIP Callback: New incoming client connection
static err_t on_tcp_accept(void* arg, struct tcp_pcb* newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }

    ClientConnection* client = allocate_client(newpcb);
    if (!client) {
        std::printf("[WARN] Client pool full! Rejecting connection.\n");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    tcp_arg(newpcb, client);
    tcp_recv(newpcb, on_tcp_recv);
    tcp_sent(newpcb, on_tcp_sent);
    tcp_err(newpcb, on_tcp_err);
    tcp_poll(newpcb, on_tcp_poll, 2);

    std::printf("[INFO] Accepted client [%zu] from %s:%u\n",
                client->index, ip4addr_ntoa(&newpcb->remote_ip), newpcb->remote_port);

    return ERR_OK;
}

// Start listening for Minecraft connections
static bool start_server(uint16_t port) {
    struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        std::printf("[ERROR] Failed to allocate tcp_pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, IP_ANY_TYPE, port);
    if (err != ERR_OK) {
        std::printf("[ERROR] Failed to bind to port %u (err: %d)\n", port, err);
        return false;
    }

    struct tcp_pcb* listen_pcb = tcp_listen_with_backlog(pcb, 4);
    if (!listen_pcb) {
        std::printf("[ERROR] Failed to listen on tcp_pcb\n");
        return false;
    }

    tcp_accept(listen_pcb, on_tcp_accept);
    std::printf("[INFO] Minecraft WaitingServer listening on port %u\n", port);
    return true;
}

int main() {
    stdio_init_all();
    sleep_ms(2000);

    std::printf("\n==========================================\n");
    std::printf("  WaitingServer: Pico W Minecraft Stub   \n");
    std::printf("==========================================\n");

    // 1. Initialize CYW43 Wi-Fi hardware and lwIP with regional regulatory channels
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_INDIA) != 0) {
        std::printf("[ERROR] Failed to initialize CYW43 hardware!\n");
        return -1;
    }
    std::printf("[INFO] CYW43 Wi-Fi driver initialized (Country: India, channels 1-13 enabled).\n");

    uint8_t pico_mac[6] = {};
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, pico_mac);
    std::printf("[INFO] Pico W Wi-Fi MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                pico_mac[0], pico_mac[1], pico_mac[2], pico_mac[3], pico_mac[4], pico_mac[5]);

    // 2. Load Configuration from Flash (or fall back to compile-time defaults)
    waiting_server::FlashConfig active_cfg = {};
    if (waiting_server::load_flash_config(active_cfg)) {
        std::printf("[CONFIG] Loaded persistent configuration from Flash:\n");
        std::printf("         SSID   : '%s'\n", active_cfg.wifi_ssid);
        std::printf("         Target : '%s:%d' (%s)\n",
                    active_cfg.target_host, active_cfg.target_port, active_cfg.target_mac);
    } else {
        std::printf("[CONFIG] No saved Flash configuration found. Using compile-time defaults.\n");
        std::strncpy(active_cfg.wifi_ssid, waiting_server::WIFI_SSID, sizeof(active_cfg.wifi_ssid) - 1);
        std::strncpy(active_cfg.wifi_password, waiting_server::WIFI_PASSWORD, sizeof(active_cfg.wifi_password) - 1);
        std::strncpy(active_cfg.target_host, waiting_server::TARGET_HOST, sizeof(active_cfg.target_host) - 1);
        active_cfg.target_port = waiting_server::TARGET_PORT;
        std::strncpy(active_cfg.target_mac, waiting_server::TARGET_MAC, sizeof(active_cfg.target_mac) - 1);
    }

    // 3. Attempt connection to Wi-Fi Station with retry loop
    bool wifi_connected = false;
    if (active_cfg.wifi_ssid[0] != '\0') {
        cyw43_arch_enable_sta_mode();
        if (netif_default != nullptr) {
            netif_set_hostname(netif_default, "WaitingServer");
        }

        std::printf("[INFO] Connecting to Wi-Fi '%s'...\n", active_cfg.wifi_ssid);

        for (int attempt = 1; attempt <= 3 && !wifi_connected; ++attempt) {
            std::printf("[INFO] Wi-Fi connection attempt %d/3...\n", attempt);

            // Attempt 1st mode: WPA2 Mixed
            int status = cyw43_arch_wifi_connect_timeout_ms(
                active_cfg.wifi_ssid,
                active_cfg.wifi_password,
                CYW43_AUTH_WPA2_MIXED_PSK,
                10000
            );

            if (status == 0) {
                wifi_connected = true;
                break;
            }

            std::printf("[WARN] Attempt %d: WPA2 Mixed failed (code: %d). Retrying with WPA TKIP...\n", attempt, status);

            // Attempt 2nd mode: WPA TKIP
            status = cyw43_arch_wifi_connect_timeout_ms(
                active_cfg.wifi_ssid,
                active_cfg.wifi_password,
                CYW43_AUTH_WPA_TKIP_PSK,
                10000
            );

            if (status == 0) {
                wifi_connected = true;
                break;
            }

            std::printf("[WARN] Attempt %d: WPA TKIP failed (code: %d). Retrying with WPA2 AES...\n", attempt, status);

            // Attempt 3rd mode: WPA2 AES
            status = cyw43_arch_wifi_connect_timeout_ms(
                active_cfg.wifi_ssid,
                active_cfg.wifi_password,
                CYW43_AUTH_WPA2_AES_PSK,
                10000
            );

            if (status == 0) {
                wifi_connected = true;
                break;
            }

            std::printf("[WARN] Attempt %d: WPA2 AES failed (code: %d).\n", attempt, status);

            if (attempt < 3) {
                cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
                sleep_ms(1500);
            }
        }
    }

    // 4. Branch based on Wi-Fi connection result
    if (wifi_connected) {
        g_run_mode = RunMode::ServerRunning;

        if (netif_default != nullptr) {
            const ip4_addr_t* ip  = netif_ip4_addr(netif_default);
            const ip4_addr_t* net = netif_ip4_netmask(netif_default);
            const ip4_addr_t* gw  = netif_ip4_gw(netif_default);

            std::printf("\n==========================================\n");
            std::printf(">>> Wi-Fi Connected! <<<\n");
            std::printf("Device Name: WaitingServer\n");
            std::printf("IP Address : %s\n", ip4addr_ntoa(ip));
            std::printf("Netmask    : %s\n", ip4addr_ntoa(net));
            std::printf("Gateway    : %s\n", ip4addr_ntoa(gw));
            std::printf("==========================================\n\n");
        }

        // Initialize target checker with active config
        g_target_checker.init(active_cfg.target_host, active_cfg.target_port, active_cfg.target_mac);

        // Start Minecraft TCP server on port 25565
        if (!start_server(waiting_server::SERVER_PORT)) {
            std::printf("[ERROR] Failed to start Minecraft TCP server.\n");
            return -1;
        }
    } else {
        // Start Captive Portal Hotspot
        g_run_mode = RunMode::CaptivePortal;
        cyw43_arch_disable_sta_mode();

        std::printf("\n==========================================\n");
        std::printf(">>> STARTING CAPTIVE PORTAL HOTSPOT <<<\n");
        std::printf("Hotspot SSID : WaitingServer-Setup\n");
        std::printf("Setup URL    : http://192.168.4.1\n");
        std::printf("LED Pattern  : Double-blink (200ms on, 200ms off, 200ms on, 1000ms off)\n");
        std::printf("==========================================\n\n");

        cyw43_arch_enable_ap_mode("WaitingServer-Setup", nullptr, CYW43_AUTH_OPEN);

        ip4_addr_t ap_ip, ap_client, ap_mask;
        IP4_ADDR(&ap_ip, 192, 168, 4, 1);
        IP4_ADDR(&ap_client, 192, 168, 4, 2);
        IP4_ADDR(&ap_mask, 255, 255, 255, 0);

        g_dhcp_server.init(ap_ip, ap_client, ap_mask);
        g_dns_server.init(ap_ip);
        g_http_server.init(active_cfg);
    }

    // 5. Main event loop (polling mode)
    uint32_t last_log_time = to_ms_since_boot(get_absolute_time());
    uint32_t uptime_seconds = 0;

    while (true) {
        cyw43_arch_poll();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (g_run_mode == RunMode::CaptivePortal) {
            // Blink LED: 200ms on, 200ms off, 200ms on, 1000ms off
            update_led(now, LedPattern::CaptivePortal);

            // Poll HTTP server for reboot requests
            g_http_server.poll(now);

        } else { // RunMode::ServerRunning
            // Blink LED: 1000ms on, 1000ms off
            update_led(now, LedPattern::ServerRunning);

            // Check if any clients are currently connected and waiting
            bool has_waiting_clients = false;
            for (size_t i = 0; i < waiting_server::MAX_CLIENTS; ++i) {
                if (g_clients[i].is_active &&
                    (g_clients[i].state == ClientConnection::State::Configuration ||
                     g_clients[i].state == ClientConnection::State::Play)) {
                    has_waiting_clients = true;
                    break;
                }
            }

            // Poll target server reachability
            g_target_checker.poll(now, has_waiting_clients);

            // If target server is online, redirect any players in Play state
            if (g_target_checker.is_online) {
                for (size_t i = 0; i < waiting_server::MAX_CLIENTS; ++i) {
                    if (g_clients[i].is_active &&
                        g_clients[i].state == ClientConnection::State::Play &&
                        !g_clients[i].sending_play_queue &&
                        !g_clients[i].close_after_sent) {
                        send_play_transfer(&g_clients[i]);
                    }
                }
            }

            // Periodic keep-alive and action bar for Play-state clients
            for (size_t i = 0; i < waiting_server::MAX_CLIENTS; ++i) {
                if (g_clients[i].is_active &&
                    g_clients[i].state == ClientConnection::State::Play &&
                    !g_clients[i].sending_play_queue &&
                    !g_clients[i].close_after_sent) {
                    if (now - g_clients[i].last_keepalive_sent_ms >= 5000) {
                        g_clients[i].last_keepalive_sent_ms = now;
                        send_play_keepalive(&g_clients[i], now);
                    }
                    if (now - g_clients[i].last_actionbar_sent_ms >= 2000) {
                        g_clients[i].last_actionbar_sent_ms = now;
                        if (g_target_checker.is_online) {
                            send_play_action_bar(&g_clients[i], "§a● Main server is online! Connecting...");
                        } else {
                            send_play_action_bar(&g_clients[i], "§c◌ Main server is starting...");
                        }
                    }
                }
            }

            // Status log every 10 seconds
            if (now - last_log_time >= 10000) {
                last_log_time = now;
                uptime_seconds += 10;
                char ip_buf[16] = "0.0.0.0";
                if (netif_default != nullptr) {
                    ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_buf, sizeof(ip_buf));
                }
                std::printf("[STATUS] IP: %s | Uptime: %lu s | Target: %s (%s)\n",
                            ip_buf,
                            static_cast<unsigned long>(uptime_seconds),
                            g_target_checker.is_online ? "ONLINE" : "OFFLINE",
                            g_target_checker.target_host);
            }
        }

        sleep_ms(1);
    }

    return 0;
}