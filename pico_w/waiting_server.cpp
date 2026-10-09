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

#include "config.hpp"
#include "flash_config.hpp"
#include "dhcp_server.hpp"
#include "dns_server.hpp"
#include "http_server.hpp"
#include "bootsel.hpp"
#include "packet_reader.hpp"
#include "packet_writer.hpp"
#include "server_status.hpp"
#include "config_packets.hpp"
#include "play_packets.hpp"
#include "target_checker.hpp"
#include "client_connection.hpp"
#include "entity_sync.hpp"


// Global services
static waiting_server::TargetChecker g_target_checker;
static waiting_server::DhcpServer    g_dhcp_server;
static waiting_server::DnsServer     g_dns_server;
static waiting_server::HttpServer    g_http_server;
static waiting_server::FlashConfig   g_active_config;

enum class RunMode {
    CaptivePortal, // Wi-Fi Setup Hotspot active
    ServerRunning  // Connected to Wi-Fi & Minecraft WaitingServer listening
};

static RunMode g_run_mode = RunMode::CaptivePortal;

enum class LedPattern {
    CaptivePortal, // on 1000ms, off 1000ms (2000ms cycle)
    ServerRunning, // on 200ms, off 200ms, on 200ms, off 1000ms (1600ms cycle)
    ButtonHeld     // on 80ms, off 80ms (160ms cycle) - rapid strobe
};

// Non-blocking LED pattern generator
static void update_led(uint32_t now_ms, LedPattern pattern) {
    static bool current_state = false;
    bool new_state = false;

    if (pattern == LedPattern::ButtonHeld) {
        uint32_t phase = now_ms % 160;
        new_state = (phase < 80);
    } else if (pattern == LedPattern::ServerRunning) {
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
    } else { // CaptivePortal
        uint32_t phase = now_ms % 2000;
        new_state = (phase < 1000);
    }

    if (new_state != current_state) {
        current_state = new_state;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, new_state ? 1 : 0);
    }
}

// Forward declaration
using waiting_server::ClientConnection;
static void close_client(ClientConnection* client);
static void pump_tx(ClientConnection* client);
static void advance_config_queue(ClientConnection* client);
static void advance_play_queue(ClientConnection* client);
static void send_play_transfer(ClientConnection* client);
static void send_play_keepalive(ClientConnection* client, uint32_t now);
static void send_play_action_bar(ClientConnection* client, std::string_view text);

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
    if (client->is_active && client->state == ClientConnection::State::Play && client->is_spawned_in_play) {
        waiting_server::broadcast_player_disconnect(client, g_clients, waiting_server::MAX_CLIENTS);
    }
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
        } else if (client->play_packet_index == 12) { // Player Info Update (ID 0x47)
            waiting_server::PacketWriter writer(client->tx_buffer);
            if (writer.write_varint(0x47) &&
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
        } else if (client->play_packet_index == 19) { // Set Chunk Cache Center (ID 0x60)
            waiting_server::PacketWriter writer(client->tx_buffer);
            if (writer.write_varint(0x60) &&
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
        // Send Entity Metadata (0x65) to show player skin layers and finalize spawn
        waiting_server::PacketWriter writer(client->tx_buffer);
        int32_t entity_id = 300 + static_cast<int32_t>(client->index);
        if (writer.write_varint(0x65) &&
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
        client->is_spawned_in_play = true;
        std::printf("[PLAY] Player '%s' spawned in lobby world! (X=%.1f, Y=%.1f, Z=%.1f)\n",
                    client->player_name, waiting_server::SPAWN_X, waiting_server::SPAWN_Y, waiting_server::SPAWN_Z);

        // Cross-spawn with other players already in Play state
        waiting_server::broadcast_player_spawn(client, g_clients, waiting_server::MAX_CLIENTS);

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

// Sends a Play KeepAlive packet (0x2D)
static void send_play_keepalive(ClientConnection* client, uint32_t now) {
    if (!client || !client->pcb || client->sending_play_queue) return;
    uint64_t keepalive_id = static_cast<uint64_t>(now);
    waiting_server::PacketWriter writer(client->tx_buffer);
    if (writer.write_varint(0x2D) && writer.write_ulong(keepalive_id)) {
        auto resp = writer.finalize();
        if (resp) {
            send_dynamic_packet(client, resp->data(), resp->size(), false);
        }
    }
}

// Sends an Action Bar message to the player (Packet ID 0x59 in Protocol 777)
static void send_play_action_bar(ClientConnection* client, std::string_view text) {
    if (!client || !client->pcb || client->sending_play_queue) return;
    waiting_server::PacketWriter writer(client->tx_buffer);
    if (writer.write_varint(0x59) &&
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

// Sends the Play Transfer packet (0x84) to redirect player to primary server
static void send_play_transfer(ClientConnection* client) {
    if (!client || !client->pcb || client->sending_play_queue) return;
    const char* transfer_target = g_target_checker.target_ip_str[0] != '\0' ?
        g_target_checker.target_ip_str : g_target_checker.target_host;

    std::printf("[TRANSFER] Primary server ONLINE! Transferring '%s' to %s:%d\n",
                client->player_name, transfer_target, g_target_checker.target_port);
    waiting_server::PacketWriter writer(client->tx_buffer);
    if (writer.write_varint(0x84) &&
        writer.write_string(transfer_target) &&
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

                const uint8_t* prefix_ptr = g_target_checker.is_online ?
                    waiting_server::STATUS_PREFIX_ONLINE : waiting_server::STATUS_PREFIX_OFFLINE;
                size_t prefix_len = g_target_checker.is_online ?
                    waiting_server::STATUS_PREFIX_ONLINE_SIZE : waiting_server::STATUS_PREFIX_OFFLINE_SIZE;
                const uint8_t* suffix_ptr = g_target_checker.is_online ?
                    waiting_server::STATUS_SUFFIX_ONLINE : waiting_server::STATUS_SUFFIX_OFFLINE;
                size_t suffix_len = g_target_checker.is_online ?
                    waiting_server::STATUS_SUFFIX_ONLINE_SIZE : waiting_server::STATUS_SUFFIX_OFFLINE_SIZE;

                const char* motd_src = g_active_config.motd[0] != '\0' ?
                    g_active_config.motd : "❄ WaitingServer ✦ Pico W";
                char motd_escaped[96];
                size_t motd_len = 0;
                for (size_t i = 0; motd_src[i] != '\0' && motd_len < sizeof(motd_escaped) - 2; ++i) {
                    char c = motd_src[i];
                    if (c == '"' || c == '\\') {
                        motd_escaped[motd_len++] = '\\';
                    }
                    motd_escaped[motd_len++] = c;
                }
                motd_escaped[motd_len] = '\0';

                size_t json_len = prefix_len + motd_len + suffix_len;
                size_t json_varint_len = waiting_server::varint_size(static_cast<int32_t>(json_len));
                size_t payload_len = 1 /* packet_id 0x00 */ + json_varint_len + json_len;

                size_t offset = 0;
                // 1. Packet length VarInt
                offset += waiting_server::write_varint(std::span<std::byte>(client->tx_buffer).subspan(offset), static_cast<int32_t>(payload_len));
                // 2. Packet ID (0x00)
                offset += waiting_server::write_varint(std::span<std::byte>(client->tx_buffer).subspan(offset), 0x00);
                // 3. JSON String Length VarInt
                offset += waiting_server::write_varint(std::span<std::byte>(client->tx_buffer).subspan(offset), static_cast<int32_t>(json_len));
                // 4. JSON Prefix
                std::memcpy(client->tx_buffer.data() + offset, prefix_ptr, prefix_len);
                offset += prefix_len;
                // 5. Dynamic MOTD Line 1
                std::memcpy(client->tx_buffer.data() + offset, motd_escaped, motd_len);
                offset += motd_len;

                err_t err = tcp_write(client->pcb, client->tx_buffer.data(), static_cast<u16_t>(offset), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
                if (err != ERR_OK) {
                    std::printf("[ERROR] tcp_write status header failed: %d\n", err);
                    return false;
                }

                client->tx_stream_ptr = suffix_ptr;
                client->tx_stream_remaining = suffix_len;
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
            } else if (packet_id == 0x0B || packet_id == 0x0C || packet_id == 0x08) { // Chunk Batch Received
                return true;
            } else if (packet_id == 0x1C) { // KeepAlive response
                return true;
            } else if (packet_id == 0x1E) { // Move Player Pos
                auto x_opt = waiting_server::read_double(payload);
                auto y_opt = waiting_server::read_double(payload);
                auto z_opt = waiting_server::read_double(payload);
                auto flag_opt = waiting_server::read_byte(payload);
                if (x_opt && y_opt && z_opt && flag_opt) {
                    client->x = *x_opt;
                    client->y = *y_opt;
                    client->z = *z_opt;
                    client->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                    waiting_server::broadcast_player_movement(client, g_clients, waiting_server::MAX_CLIENTS);
                }
                return true;
            } else if (packet_id == 0x1F) { // Move Player PosRot
                auto x_opt = waiting_server::read_double(payload);
                auto y_opt = waiting_server::read_double(payload);
                auto z_opt = waiting_server::read_double(payload);
                auto yrot_opt = waiting_server::read_float(payload);
                auto xrot_opt = waiting_server::read_float(payload);
                auto flag_opt = waiting_server::read_byte(payload);
                if (x_opt && y_opt && z_opt && yrot_opt && xrot_opt && flag_opt) {
                    client->x = *x_opt;
                    client->y = *y_opt;
                    client->z = *z_opt;
                    client->yrot = *yrot_opt;
                    client->xrot = *xrot_opt;
                    client->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                    waiting_server::broadcast_player_movement(client, g_clients, waiting_server::MAX_CLIENTS);
                }
                return true;
            } else if (packet_id == 0x20) { // Move Player Rot
                auto yrot_opt = waiting_server::read_float(payload);
                auto xrot_opt = waiting_server::read_float(payload);
                auto flag_opt = waiting_server::read_byte(payload);
                if (yrot_opt && xrot_opt && flag_opt) {
                    client->yrot = *yrot_opt;
                    client->xrot = *xrot_opt;
                    client->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                    waiting_server::broadcast_player_movement(client, g_clients, waiting_server::MAX_CLIENTS);
                }
                return true;
            } else if (packet_id == 0x21) { // Move Player StatusOnly
                auto flag_opt = waiting_server::read_byte(payload);
                if (flag_opt) {
                    client->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                }
                return true;
            } else if (packet_id == 0x2B) { // Player Input (Sneak)
                auto flag_opt = waiting_server::read_byte(payload);
                if (flag_opt) {
                    bool sneaking = (static_cast<uint8_t>(*flag_opt) & 0x20) != 0;
                    if (sneaking != client->is_sneaking) {
                        client->is_sneaking = sneaking;
                        waiting_server::broadcast_player_sneak(client, g_clients, waiting_server::MAX_CLIENTS);
                    }
                }
                return true;
            }
            // Allow other play packets
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
        std::printf("[ERROR] RX buffer overflow (%zu + %u > %zu)! Dropping client.\n",
                    client->rx_len, p->tot_len, client->rx_buffer.size());
        pbuf_free(p);
        close_client(client);
        return ERR_OK;
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
        if (client->is_active && client->state == ClientConnection::State::Play && client->is_spawned_in_play) {
            waiting_server::broadcast_player_disconnect(client, g_clients, waiting_server::MAX_CLIENTS);
        }
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

static struct tcp_pcb* g_server_pcb = nullptr;

// Stop listening for Minecraft connections and cleanly disconnect any active clients
static void stop_server() {
    for (size_t i = 0; i < waiting_server::MAX_CLIENTS; ++i) {
        if (g_clients[i].is_active) {
            close_client(&g_clients[i]);
        }
    }
    if (g_server_pcb) {
        tcp_close(g_server_pcb);
        g_server_pcb = nullptr;
        std::printf("[INFO] Minecraft WaitingServer stopped.\n");
    }
}

// Start listening for Minecraft connections
static bool start_server(uint16_t port) {
    if (g_server_pcb) {
        stop_server();
    }

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

    g_server_pcb = tcp_listen_with_backlog(pcb, 4);
    if (!g_server_pcb) {
        std::printf("[ERROR] Failed to listen on tcp_pcb\n");
        return false;
    }

    tcp_accept(g_server_pcb, on_tcp_accept);
    std::printf("[INFO] Minecraft WaitingServer listening on port %u\n", port);
    return true;
}

// Transition into Captive Portal mode
static void start_captive_portal(const char* reason = nullptr) {
    g_run_mode = RunMode::CaptivePortal;

    // 1. Stop Minecraft server and any connected clients
    stop_server();

    // 2. Disconnect Wi-Fi Station
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_disable_sta_mode();

    // 3. Start Access Point mode
    std::printf("\n==========================================\n");
    std::printf(">>> STARTING CAPTIVE PORTAL <<<\n");
    std::printf("Hotspot SSID : WaitingServer-Setup\n");
    std::printf("Setup URL    : http://192.168.4.1\n");
    if (reason && reason[0] != '\0') {
        std::printf("Trigger/Info : %s\n", reason);
    }
    std::printf("==========================================\n\n");

    cyw43_arch_enable_ap_mode("WaitingServer-Setup", nullptr, CYW43_AUTH_OPEN);

    ip4_addr_t ap_ip, ap_client, ap_mask;
    IP4_ADDR(&ap_ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_client, 192, 168, 4, 2);
    IP4_ADDR(&ap_mask, 255, 255, 255, 0);

    g_dhcp_server.init(ap_ip, ap_client, ap_mask);
    g_dns_server.init(ap_ip);
    g_http_server.init(g_active_config, (reason && reason[0] != '\0') ? reason : nullptr);
}

int main() {
    stdio_init_all();

    // Check if BOOTSEL button is held at boot (during 2-second boot window)
    bool force_portal = false;
    uint32_t boot_check_start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - boot_check_start < 2000) {
        if (waiting_server::is_bootsel_pressed()) {
            force_portal = true;
            std::printf("[BOOT] BOOTSEL held at startup! Forcing Captive Portal mode.\n");
            break;
        }
        sleep_ms(50);
    }

    std::printf("----- Pico Minecraft 26.3 Waiting Server -----\n\n");

    // TODO: Make wifi connection compatible with other countries too
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

    // 2. Load Configuration from Flash
    bool config_loaded = waiting_server::load_flash_config(g_active_config);
    if (config_loaded) {
        std::printf("[CONFIG] Loaded persistent configuration from Flash:\n");
        std::printf("         SSID     : '%s'\n", g_active_config.wifi_ssid);
        std::printf("         Hostname : '%s'\n", g_active_config.hostname);
        std::printf("         Auth     : 0x%08lX\n", static_cast<unsigned long>(g_active_config.wifi_auth));
        std::printf("         Attempts : %u\n", g_active_config.connect_attempts);
        std::printf("         MOTD     : '%s'\n", g_active_config.motd);
        std::printf("         Target   : '%s:%d' (%s)\n",
                    g_active_config.target_host, g_active_config.target_port, g_active_config.target_mac);
    } else {
        std::printf("[CONFIG] No saved Flash configuration found. Initializing setup defaults.\n");
        waiting_server::init_default_config(g_active_config);
    }

    // 3. Attempt connection to Wi-Fi Station with retry loop
    bool wifi_connected = false;
    char fail_reason[160] = {};

    if (force_portal) {
        std::snprintf(fail_reason, sizeof(fail_reason), "Manually triggered via BOOTSEL button at boot.");
    } else if (g_active_config.wifi_ssid[0] != '\0') {
        cyw43_arch_enable_sta_mode();
        if (netif_default != nullptr) {
            const char* host = g_active_config.hostname[0] != '\0' ? g_active_config.hostname : "WaitingServer";
            netif_set_hostname(netif_default, host);
        }

        int max_attempts = g_active_config.connect_attempts;
        if (max_attempts < 1) max_attempts = 1;
        if (max_attempts > 5) max_attempts = 5;

        uint32_t auth_mode = g_active_config.wifi_auth;
        if (auth_mode == 0 && g_active_config.wifi_password[0] != '\0') {
            auth_mode = CYW43_AUTH_WPA2_MIXED_PSK;
        }

        std::printf("[INFO] Connecting to Wi-Fi '%s' (Auth: 0x%08lX, Max Attempts: %d)...\n",
                    g_active_config.wifi_ssid, static_cast<unsigned long>(auth_mode), max_attempts);

        int last_status = 0;
        for (int attempt = 1; attempt <= max_attempts && !wifi_connected; ++attempt) {
            std::printf("[INFO] Wi-Fi connection attempt %d/%d...\n", attempt, max_attempts);

            last_status = cyw43_arch_wifi_connect_timeout_ms(
                g_active_config.wifi_ssid,
                g_active_config.wifi_password,
                auth_mode,
                15000
            );

            if (last_status == 0) {
                wifi_connected = true;
                break;
            }

            std::printf("[WARN] Attempt %d/%d failed (code: %d).\n", attempt, max_attempts, last_status);

            if (attempt < max_attempts) {
                cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
                sleep_ms(1500);
            }
        }

        if (!wifi_connected) {
            const char* err_desc = "Unknown error";
            if (last_status == PICO_ERROR_TIMEOUT) {
                err_desc = "Timed out (Network out of range or not found)";
            } else if (last_status == PICO_ERROR_BADAUTH) {
                err_desc = "Authentication failed (Wrong password or auth type)";
            } else if (last_status == PICO_ERROR_CONNECT_FAILED) {
                err_desc = "Connection failed (Association rejected)";
            } else if (last_status == PICO_ERROR_GENERIC) {
                err_desc = "Generic network failure";
            }

            std::snprintf(fail_reason, sizeof(fail_reason),
                          "Failed to connect to '%s' after %d attempt%s (Code %d: %s).",
                          g_active_config.wifi_ssid, max_attempts, (max_attempts > 1 ? "s" : ""),
                          last_status, err_desc);
            std::printf("[ERROR] %s\n", fail_reason);
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
            std::printf("Device Name: %s\n", g_active_config.hostname[0] != '\0' ? g_active_config.hostname : "WaitingServer");
            std::printf("IP Address : %s\n", ip4addr_ntoa(ip));
            std::printf("Netmask    : %s\n", ip4addr_ntoa(net));
            std::printf("Gateway    : %s\n", ip4addr_ntoa(gw));
            std::printf("==========================================\n\n");
        }

        // Initialize target checker with active config
        g_target_checker.init(g_active_config.target_host, g_active_config.target_port, g_active_config.target_mac);

        // Start Minecraft TCP server on port 25565
        if (!start_server(waiting_server::SERVER_PORT)) {
            std::printf("[ERROR] Failed to start Minecraft TCP server.\n");
            return -1;
        }
    } else {
        start_captive_portal(fail_reason[0] != '\0' ? fail_reason : nullptr);
    }

    // 5. Main event loop (polling mode)
    uint32_t last_log_time = to_ms_since_boot(get_absolute_time());
    uint32_t uptime_seconds = 0;
    uint32_t bootsel_press_start_ms = 0;

    while (true) {
        cyw43_arch_poll();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Check onboard BOOTSEL button
        if (waiting_server::is_bootsel_pressed()) {
            if (bootsel_press_start_ms == 0) {
                bootsel_press_start_ms = now;
            } else {
                uint32_t held_ms = now - bootsel_press_start_ms;
                if (held_ms >= 3000) {
                    if (g_run_mode == RunMode::ServerRunning) {
                        std::printf("[BUTTON] BOOTSEL held for 3 seconds! Switching to Captive Portal...\n");
                        start_captive_portal("Manually triggered via BOOTSEL button (3s hold)");
                    } else if (g_run_mode == RunMode::CaptivePortal) {
                        std::printf("[BUTTON] BOOTSEL held for 3 seconds in Captive Portal! Rebooting...\n");
                        watchdog_reboot(0, 0, 0);
                    }
                    bootsel_press_start_ms = 0;
                }
            }
        } else {
            bootsel_press_start_ms = 0;
        }

        // LED feedback: rapid strobe while button is held, else mode-specific pattern
        if (bootsel_press_start_ms != 0 && (now - bootsel_press_start_ms) >= 300) {
            update_led(now, LedPattern::ButtonHeld);
        } else if (g_run_mode == RunMode::CaptivePortal) {
            update_led(now, LedPattern::CaptivePortal);
        } else {
            update_led(now, LedPattern::ServerRunning);
        }

        if (g_run_mode == RunMode::CaptivePortal) {
            g_http_server.poll(now);
        } else {

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
            // if (now - last_log_time >= 10000) {
            //     last_log_time = now;
            //     uptime_seconds += 10;
            //     char ip_buf[16] = "0.0.0.0";
            //     if (netif_default != nullptr) {
            //         ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_buf, sizeof(ip_buf));
            //     }
            //     std::printf("[STATUS] IP: %s | Uptime: %lu s | Target: %s (%s)\n",
            //                 ip_buf,
            //                 static_cast<unsigned long>(uptime_seconds),
            //                 g_target_checker.is_online ? "ONLINE" : "OFFLINE",
            //                 g_target_checker.target_host);
            // }
        }

        sleep_ms(1);
    }

    return 0;
}