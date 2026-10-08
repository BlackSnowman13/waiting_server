#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <span>
#include <cmath>
#include <string_view>
#include "lwip/tcp.h"
#include "client_connection.hpp"
#include "packet_writer.hpp"

namespace waiting_server {

// Helper: Sends a complete spawn sequence for 'target' to 'recipient'
// Packets sent:
// 1. Player Info Update (0x46) - Adds player to tab list with offline default skin
// 2. Add Entity (0x01) - Spawns player entity model
// 3. Set Entity Data / Metadata (0x63) - Outer skin layers (0x7F) + current sneak state & pose
// 4. Rotate Head (0x53) - Sets looking direction
inline void send_spawn_player_packets(ClientConnection* recipient, const ClientConnection* target) {
    if (!recipient || !recipient->pcb || !target) return;

    int32_t entity_id = 300 + static_cast<int32_t>(target->index);

    // 1. Player Info Update (0x46)
    std::array<std::byte, 128> info_buf;
    PacketWriter info_writer(info_buf);
    if (info_writer.write_varint(0x46) &&
        info_writer.write_byte(std::byte{0xFF}) && // All actions mask
        info_writer.write_varint(1) &&              // Count: 1
        info_writer.write_uuid(target->player_uuid) &&
        info_writer.write_string(std::string_view(target->player_name)) &&
        info_writer.write_varint(0) &&              // 0 properties (offline skin)
        info_writer.write_bool(false) &&            // Chat session
        info_writer.write_varint(0) &&              // GameMode: 0 (Survival)
        info_writer.write_bool(true) &&             // Listed
        info_writer.write_varint(0) &&              // Latency: 0
        info_writer.write_bool(false) &&            // Display name
        info_writer.write_varint(0) &&              // List order
        info_writer.write_bool(false)) {            // Show hat
        auto info_span = info_writer.finalize();
        if (info_span && tcp_sndbuf(recipient->pcb) >= info_span->size()) {
            tcp_write(recipient->pcb, info_span->data(), static_cast<u16_t>(info_span->size()), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        }
    }

    // 2. Add Entity (0x01)
    std::array<std::byte, 128> spawn_buf;
    PacketWriter spawn_writer(spawn_buf);
    if (spawn_writer.write_varint(0x01) &&
        spawn_writer.write_varint(entity_id) &&
        spawn_writer.write_uuid(target->player_uuid) &&
        spawn_writer.write_varint(156) && // Entity Type: Player (156 in Protocol 776 / 26.2)
        spawn_writer.write_double(target->x) &&
        spawn_writer.write_double(target->y) &&
        spawn_writer.write_double(target->z) &&
        spawn_writer.write_byte(std::byte{0x00}) && // Velocity: LpVec3 zero = 0x00
        spawn_writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(target->xrot * 256.0f / 360.0f))) &&
        spawn_writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(target->yrot * 256.0f / 360.0f))) &&
        spawn_writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(target->yrot * 256.0f / 360.0f))) && // Head Yaw
        spawn_writer.write_varint(0)) { // Data: 0
        auto spawn_span = spawn_writer.finalize();
        if (spawn_span && tcp_sndbuf(recipient->pcb) >= spawn_span->size()) {
            tcp_write(recipient->pcb, spawn_span->data(), static_cast<u16_t>(spawn_span->size()), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        }
    }

    // 3. Set Entity Data / Metadata (0x63)
    std::array<std::byte, 64> meta_buf;
    PacketWriter meta_writer(meta_buf);
    if (meta_writer.write_varint(0x63) &&
        meta_writer.write_varint(entity_id) &&
        // Index 0: Shared flags (Byte) - bit 1 (0x02) is shift key down
        meta_writer.write_byte(std::byte{0}) &&
        meta_writer.write_varint(0) &&
        meta_writer.write_byte(static_cast<std::byte>(target->is_sneaking ? 0x02 : 0x00)) &&
        // Index 6: Pose (Type 20 = Pose) - 5 = CROUCHING, 0 = STANDING
        meta_writer.write_byte(std::byte{6}) &&
        meta_writer.write_varint(20) &&
        meta_writer.write_varint(target->is_sneaking ? 5 : 0) &&
        // Index 16: Skin display mask (Byte) -> Enable all outer skin layers (cape, jacket, sleeves, pants, hat)
        meta_writer.write_byte(std::byte{16}) &&
        meta_writer.write_varint(0) &&
        meta_writer.write_byte(std::byte{0x7F}) &&
        // End of metadata terminator
        meta_writer.write_byte(std::byte{0xFF})) {
        auto meta_span = meta_writer.finalize();
        if (meta_span && tcp_sndbuf(recipient->pcb) >= meta_span->size()) {
            tcp_write(recipient->pcb, meta_span->data(), static_cast<u16_t>(meta_span->size()), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        }
    }

    // 4. Rotate Head (0x53)
    std::array<std::byte, 32> head_buf;
    PacketWriter head_writer(head_buf);
    if (head_writer.write_varint(0x53) &&
        head_writer.write_varint(entity_id) &&
        head_writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(target->yrot * 256.0f / 360.0f)))) {
        auto head_span = head_writer.finalize();
        if (head_span && tcp_sndbuf(recipient->pcb) >= head_span->size()) {
            tcp_write(recipient->pcb, head_span->data(), static_cast<u16_t>(head_span->size()), TCP_WRITE_FLAG_COPY);
        }
    }

    tcp_output(recipient->pcb);
}

// Cross-spawns a newly spawned player with all other active players in Play state
inline void broadcast_player_spawn(ClientConnection* new_player, ClientConnection* clients, size_t max_clients) {
    if (!new_player || !new_player->is_spawned_in_play) return;

    for (size_t i = 0; i < max_clients; ++i) {
        auto& other = clients[i];
        if (other.is_active && other.state == ClientConnection::State::Play &&
            other.is_spawned_in_play && other.pcb && &other != new_player) {
            // Send new_player's entity to other
            send_spawn_player_packets(&other, new_player);
            // Send other's entity to new_player
            send_spawn_player_packets(new_player, &other);
        }
    }
}

// Broadcasts movement (Teleport Entity 0x7D + Rotate Head 0x53) with deadband thresholding
inline void broadcast_player_movement(ClientConnection* sender, ClientConnection* clients, size_t max_clients) {
    if (!sender || !sender->is_active || sender->state != ClientConnection::State::Play || !sender->is_spawned_in_play) {
        return;
    }

    double dx = sender->x - sender->last_broadcast_x;
    double dy = sender->y - sender->last_broadcast_y;
    double dz = sender->z - sender->last_broadcast_z;
    float dyrot = std::abs(sender->yrot - sender->last_broadcast_yrot);
    float dxrot = std::abs(sender->xrot - sender->last_broadcast_xrot);

    // Deadband threshold: delta position > 0.01 blocks or delta angle > 1.0 degree
    if ((dx * dx + dy * dy + dz * dz) < 0.0001 && dyrot < 1.0f && dxrot < 1.0f) {
        return;
    }

    sender->last_broadcast_x = sender->x;
    sender->last_broadcast_y = sender->y;
    sender->last_broadcast_z = sender->z;
    sender->last_broadcast_yrot = sender->yrot;
    sender->last_broadcast_xrot = sender->xrot;

    int32_t entity_id = 300 + static_cast<int32_t>(sender->index);

    // Build Teleport Entity (0x7D)
    std::array<std::byte, 128> tele_buf;
    PacketWriter tele_writer(tele_buf);
    if (!tele_writer.write_varint(0x7D) ||
        !tele_writer.write_varint(entity_id) ||
        !tele_writer.write_double(sender->x) ||
        !tele_writer.write_double(sender->y) ||
        !tele_writer.write_double(sender->z) ||
        !tele_writer.write_double(0.0) ||
        !tele_writer.write_double(0.0) ||
        !tele_writer.write_double(0.0) ||
        !tele_writer.write_float(sender->yrot) ||
        !tele_writer.write_float(sender->xrot) ||
        !tele_writer.write_byte(std::byte{0x00}) || // Relative bitmask int (4 bytes 0)
        !tele_writer.write_byte(std::byte{0x00}) ||
        !tele_writer.write_byte(std::byte{0x00}) ||
        !tele_writer.write_byte(std::byte{0x00}) ||
        !tele_writer.write_bool(sender->on_ground)) {
        return;
    }
    auto tele_span = tele_writer.finalize();
    if (!tele_span) return;

    // Build Rotate Head (0x53)
    std::array<std::byte, 32> rot_buf;
    PacketWriter rot_writer(rot_buf);
    if (!rot_writer.write_varint(0x53) ||
        !rot_writer.write_varint(entity_id) ||
        !rot_writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(sender->yrot * 256.0f / 360.0f)))) {
        return;
    }
    auto rot_span = rot_writer.finalize();
    if (!rot_span) return;

    size_t total_size = tele_span->size() + rot_span->size();

    for (size_t i = 0; i < max_clients; ++i) {
        auto& other = clients[i];
        if (other.is_active && other.state == ClientConnection::State::Play &&
            other.is_spawned_in_play && other.pcb && &other != sender) {
            if (tcp_sndbuf(other.pcb) >= total_size) {
                tcp_write(other.pcb, tele_span->data(), static_cast<u16_t>(tele_span->size()), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
                tcp_write(other.pcb, rot_span->data(), static_cast<u16_t>(rot_span->size()), TCP_WRITE_FLAG_COPY);
                tcp_output(other.pcb);
            }
        }
    }
}

// Broadcasts sneak / crouch state (Set Entity Data 0x63)
inline void broadcast_player_sneak(ClientConnection* sender, ClientConnection* clients, size_t max_clients) {
    if (!sender || !sender->is_active || sender->state != ClientConnection::State::Play || !sender->is_spawned_in_play) {
        return;
    }

    int32_t entity_id = 300 + static_cast<int32_t>(sender->index);

    std::array<std::byte, 64> meta_buf;
    PacketWriter writer(meta_buf);
    if (!writer.write_varint(0x63) ||
        !writer.write_varint(entity_id) ||
        // Index 0: Shared flags (Byte) - bit 1 (0x02) = shift key down
        !writer.write_byte(std::byte{0}) ||
        !writer.write_varint(0) || // Type 0 = Byte
        !writer.write_byte(static_cast<std::byte>(sender->is_sneaking ? 0x02 : 0x00)) ||
        // Index 6: Pose (Type 20 = Pose) - 5 = CROUCHING, 0 = STANDING
        !writer.write_byte(std::byte{6}) ||
        !writer.write_varint(20) || // Type 20 = Pose
        !writer.write_varint(sender->is_sneaking ? 5 : 0) ||
        // Terminator
        !writer.write_byte(std::byte{0xFF})) {
        return;
    }
    auto meta_span = writer.finalize();
    if (!meta_span) return;

    for (size_t i = 0; i < max_clients; ++i) {
        auto& other = clients[i];
        if (other.is_active && other.state == ClientConnection::State::Play &&
            other.is_spawned_in_play && other.pcb && &other != sender) {
            if (tcp_sndbuf(other.pcb) >= meta_span->size()) {
                tcp_write(other.pcb, meta_span->data(), static_cast<u16_t>(meta_span->size()), TCP_WRITE_FLAG_COPY);
                tcp_output(other.pcb);
            }
        }
    }
}

// Broadcasts despawn / tab removal when a player disconnects
inline void broadcast_player_disconnect(ClientConnection* departing, ClientConnection* clients, size_t max_clients) {
    if (!departing || !departing->is_spawned_in_play) return;

    int32_t entity_id = 300 + static_cast<int32_t>(departing->index);

    // 1. Remove Entities (0x4D)
    std::array<std::byte, 32> ent_buf;
    PacketWriter ent_writer(ent_buf);
    if (!ent_writer.write_varint(0x4D) ||
        !ent_writer.write_varint(1) ||
        !ent_writer.write_varint(entity_id)) {
        return;
    }
    auto ent_span = ent_writer.finalize();
    if (!ent_span) return;

    // 2. Player Info Remove (0x45)
    std::array<std::byte, 32> info_buf;
    PacketWriter info_writer(info_buf);
    if (!info_writer.write_varint(0x45) ||
        !info_writer.write_varint(1) ||
        !info_writer.write_uuid(departing->player_uuid)) {
        return;
    }
    auto info_span = info_writer.finalize();
    if (!info_span) return;

    size_t total_size = ent_span->size() + info_span->size();

    for (size_t i = 0; i < max_clients; ++i) {
        auto& other = clients[i];
        if (other.is_active && other.state == ClientConnection::State::Play &&
            other.is_spawned_in_play && other.pcb && &other != departing) {
            if (tcp_sndbuf(other.pcb) >= total_size) {
                tcp_write(other.pcb, ent_span->data(), static_cast<u16_t>(ent_span->size()), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
                tcp_write(other.pcb, info_span->data(), static_cast<u16_t>(info_span->size()), TCP_WRITE_FLAG_COPY);
                tcp_output(other.pcb);
            }
        }
    }
}

} // namespace waiting_server
