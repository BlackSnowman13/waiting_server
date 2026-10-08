#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>
#include "lwip/tcp.h"
#include "config.hpp"

namespace waiting_server {

// Static Client Connection representation (Zero runtime heap allocation)
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

    // Entity / Position State
    double x = waiting_server::SPAWN_X;
    double y = waiting_server::SPAWN_Y;
    double z = waiting_server::SPAWN_Z;
    float yrot = 0.0f;
    float xrot = 0.0f;
    bool on_ground = true;
    bool is_sneaking = false;
    bool is_spawned_in_play = false;

    // Last broadcast coordinates for threshold filtering
    double last_broadcast_x = waiting_server::SPAWN_X;
    double last_broadcast_y = waiting_server::SPAWN_Y;
    double last_broadcast_z = waiting_server::SPAWN_Z;
    float last_broadcast_yrot = 0.0f;
    float last_broadcast_xrot = 0.0f;

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

        x = waiting_server::SPAWN_X;
        y = waiting_server::SPAWN_Y;
        z = waiting_server::SPAWN_Z;
        yrot = 0.0f;
        xrot = 0.0f;
        on_ground = true;
        is_sneaking = false;
        is_spawned_in_play = false;

        last_broadcast_x = waiting_server::SPAWN_X;
        last_broadcast_y = waiting_server::SPAWN_Y;
        last_broadcast_z = waiting_server::SPAWN_Z;
        last_broadcast_yrot = 0.0f;
        last_broadcast_xrot = 0.0f;

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

} // namespace waiting_server
