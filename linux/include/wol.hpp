#pragma once
#include <asio.hpp>
#include <array>
#include <string>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <algorithm>

namespace waiting_server {

inline bool parse_mac_address(const std::string& mac_str, std::array<uint8_t, 6>& mac_bytes) {
    int values[6];
    int parsed = 0;
    if (mac_str.find(':') != std::string::npos) {
        parsed = std::sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x",
                             &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    } else if (mac_str.find('-') != std::string::npos) {
        parsed = std::sscanf(mac_str.c_str(), "%x-%x-%x-%x-%x-%x",
                             &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    } else if (mac_str.size() == 12) {
        parsed = std::sscanf(mac_str.c_str(), "%02x%02x%02x%02x%02x%02x",
                             &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    }
    if (parsed != 6) return false;
    for (int i = 0; i < 6; ++i) {
        mac_bytes[i] = static_cast<uint8_t>(values[i]);
    }
    return true;
}

inline bool send_wake_on_lan(asio::io_context& io_context, const std::string& mac_str) {
    std::array<uint8_t, 6> mac = {};
    if (!parse_mac_address(mac_str, mac)) {
        std::printf("\033[1;31m[WOL ERROR]\033[0m Invalid MAC address: %s\n", mac_str.c_str());
        return false;
    }

    // Construct 102-byte Magic Packet: 6 bytes 0xFF followed by 16 repetitions of target MAC
    std::array<uint8_t, 102> packet;
    std::fill_n(packet.begin(), 6, 0xFF);
    for (size_t i = 0; i < 16; ++i) {
        std::copy(mac.begin(), mac.end(), packet.begin() + 6 + i * 6);
    }

    try {
        asio::ip::udp::socket socket(io_context);
        socket.open(asio::ip::udp::v4());
        socket.set_option(asio::socket_base::broadcast(true));
        asio::ip::udp::endpoint broadcast_ep(asio::ip::address_v4::broadcast(), 9);
        socket.send_to(asio::buffer(packet), broadcast_ep);
        std::printf("\033[1;35m[WOL]\033[0m Broadcasted Wake-on-LAN magic packet to 255.255.255.255:9 for %s\n",
                    mac_str.c_str());
        return true;
    } catch (const std::exception& e) {
        std::printf("\033[1;31m[WOL ERROR]\033[0m Failed to send UDP WoL: %s\n", e.what());
        return false;
    }
}

} // namespace waiting_server
