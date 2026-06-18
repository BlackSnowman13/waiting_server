#pragma once
#include <span>
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <algorithm>
#include <cstring>

namespace waiting_server {

inline constexpr size_t varint_size(int32_t value) {
    uint32_t uval = static_cast<uint32_t>(value);
    if (uval < 0x80) return 1;
    if (uval < 0x4000) return 2;
    if (uval < 0x200000) return 3;
    if (uval < 0x10000000) return 4;
    return 5;
}

inline size_t write_varint(std::span<std::byte> dest, int32_t value) {
    uint32_t uval = static_cast<uint32_t>(value);
    size_t bytes_written = 0;
    while (true) {
        std::byte temp = static_cast<std::byte>(uval & 0x7F);
        uval >>= 7;
        if (uval != 0) {
            temp |= static_cast<std::byte>(0x80);
        }
        if (bytes_written >= dest.size()) {
            return 0; // Overflow
        }
        dest[bytes_written++] = temp;
        if (uval == 0) {
            break;
        }
    }
    return bytes_written;
}

class PacketWriter {
public:
    explicit PacketWriter(std::span<std::byte> buffer) 
        : buffer_(buffer), offset_(5) {}

    // Resets the writer to prepare for a new packet.
    // Reserves the first 5 bytes for the packet length VarInt.
    void reset() {
        offset_ = 5;
    }

    bool write_varint(int32_t value) {
        size_t written = waiting_server::write_varint(buffer_.subspan(offset_), value);
        if (written == 0) return false;
        offset_ += written;
        return true;
    }

    bool write_ushort(uint16_t value) {
        if (buffer_.size() - offset_ < 2) return false;
        buffer_[offset_] = static_cast<std::byte>((value >> 8) & 0xFF);
        buffer_[offset_ + 1] = static_cast<std::byte>(value & 0xFF);
        offset_ += 2;
        return true;
    }

    bool write_ulong(uint64_t value) {
        if (buffer_.size() - offset_ < 8) return false;
        for (int i = 0; i < 8; ++i) {
            buffer_[offset_ + i] = static_cast<std::byte>((value >> (8 * (7 - i))) & 0xFF);
        }
        offset_ += 8;
        return true;
    }

    bool write_uuid(const std::array<std::byte, 16>& uuid) {
        if (buffer_.size() - offset_ < 16) return false;
        std::memcpy(&buffer_[offset_], uuid.data(), 16);
        offset_ += 16;
        return true;
    }

    bool write_string(std::string_view str) {
        if (!write_varint(static_cast<int32_t>(str.size()))) return false;
        if (buffer_.size() - offset_ < str.size()) return false;
        std::memcpy(&buffer_[offset_], str.data(), str.size());
        offset_ += str.size();
        return true;
    }

    // Finalizes the packet by prepending the length VarInt before the payload data.
    // Returns the final serialized span (including the length prefix).
    std::optional<std::span<const std::byte>> finalize() {
        int32_t payload_len = static_cast<int32_t>(offset_ - 5);
        size_t v_len = waiting_server::varint_size(payload_len);
        if (v_len > 5) return std::nullopt; // Error: packet header overflow
        
        size_t start_idx = 5 - v_len;
        size_t written = waiting_server::write_varint(buffer_.subspan(start_idx, v_len), payload_len);
        if (written == 0) return std::nullopt;
        
        return buffer_.subspan(start_idx, v_len + payload_len);
    }

private:
    std::span<std::byte> buffer_;
    size_t offset_;
};

} // namespace waiting_server
