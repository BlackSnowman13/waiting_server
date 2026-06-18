#pragma once
#include <span>
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <algorithm>

namespace waiting_server {

// Read a VarInt from a span. If successful, advances the span and returns the value.
inline std::optional<int32_t> read_varint(std::span<const std::byte>& data) {
    int32_t value = 0;
    int position = 0;
    
    // We copy the span to only advance it if we successfully read the VarInt
    std::span<const std::byte> temp = data;
    
    while (true) {
        if (temp.empty()) {
            return std::nullopt; // Incomplete data
        }
        
        std::byte current_byte = temp[0];
        temp = temp.subspan(1);
        
        value |= (static_cast<int32_t>(current_byte) & 0x7F) << position;
        
        if ((static_cast<int32_t>(current_byte) & 0x80) == 0) {
            break;
        }
        
        position += 7;
        
        if (position >= 35) { // VarInts are at most 5 bytes (35 bits limit)
            return std::nullopt; // Protocol error
        }
    }
    
    data = temp;
    return value;
}

// Read an unsigned short (big-endian)
inline std::optional<uint16_t> read_ushort(std::span<const std::byte>& data) {
    if (data.size() < 2) {
        return std::nullopt;
    }
    uint16_t value = (static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]);
    data = data.subspan(2);
    return value;
}

// Read a uint64_t (big-endian)
inline std::optional<uint64_t> read_ulong(std::span<const std::byte>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint8_t>(data[i]);
    }
    data = data.subspan(8);
    return value;
}

// Read a UUID (16 bytes)
inline std::optional<std::array<std::byte, 16>> read_uuid(std::span<const std::byte>& data) {
    if (data.size() < 16) {
        return std::nullopt;
    }
    std::array<std::byte, 16> uuid;
    std::copy(data.begin(), data.begin() + 16, uuid.begin());
    data = data.subspan(16);
    return uuid;
}

// Read a string (length-prefixed with a VarInt)
inline std::optional<std::string_view> read_string(std::span<const std::byte>& data) {
    std::span<const std::byte> temp = data;
    auto len_opt = read_varint(temp);
    if (!len_opt) {
        return std::nullopt;
    }
    int32_t len = *len_opt;
    if (len < 0 || temp.size() < static_cast<size_t>(len)) {
        return std::nullopt;
    }
    std::string_view sv(reinterpret_cast<const char*>(temp.data()), len);
    data = temp.subspan(len);
    return sv;
}

class PacketReader {
public:
    explicit PacketReader(std::span<const std::byte> data) : remaining_(data) {}

    std::optional<int32_t> read_varint() {
        return waiting_server::read_varint(remaining_);
    }

    std::optional<uint16_t> read_ushort() {
        return waiting_server::read_ushort(remaining_);
    }

    std::optional<uint64_t> read_ulong() {
        return waiting_server::read_ulong(remaining_);
    }

    std::optional<std::array<std::byte, 16>> read_uuid() {
        return waiting_server::read_uuid(remaining_);
    }

    std::optional<std::string_view> read_string() {
        return waiting_server::read_string(remaining_);
    }

    bool empty() const {
        return remaining_.empty();
    }

    size_t remaining_size() const {
        return remaining_.size();
    }

    std::span<const std::byte> remaining() const {
        return remaining_;
    }

private:
    std::span<const std::byte> remaining_;
};

} // namespace waiting_server
