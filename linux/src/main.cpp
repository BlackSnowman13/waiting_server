#include <asio.hpp>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <array>
#include <vector>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <chrono>
#include <algorithm>
#include <csignal>

#include "config.hpp"
#include "wol.hpp"
#include "target_checker.hpp"

#include "packet_reader.hpp"
#include "packet_writer.hpp"
#include "server_status.hpp"
#include "config_packets.hpp"
#include "play_packets.hpp"
#include "lobby_chunks.hpp"

namespace waiting_server {

class MinecraftServer;

struct OutboundItem {
    std::vector<uint8_t> dynamic_buffer;
    const uint8_t* static_ptr = nullptr;
    size_t static_size = 0;
    bool close_after = false;
};

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    enum class State : uint8_t {
        Handshake,
        Status,
        Login,
        Configuration,
        Play,
        Closed
    };

    ClientSession(asio::ip::tcp::socket socket,
                  MinecraftServer& server,
                  const Config& config,
                  std::shared_ptr<TargetChecker> checker,
                  uint32_t session_id)
        : socket_(std::move(socket))
        , server_(server)
        , config_(config)
        , target_checker_(std::move(checker))
        , session_id_(session_id)
        , state_(State::Handshake)
        , keepalive_timer_(socket_.get_executor())
        , actionbar_timer_(socket_.get_executor())
    {}

    void start() {
        do_read();
    }

    void on_target_online() {
        if (state_ == State::Play && !sending_play_queue_ && !is_transferring_) {
            send_play_transfer();
        }
    }

    void close() {
        if (state_ == State::Closed) return;
        state_ = State::Closed;
        keepalive_timer_.cancel();
        actionbar_timer_.cancel();
        std::error_code ec;
        socket_.close(ec);
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(rx_temp_buffer_),
            [this, self](const std::error_code& ec, size_t bytes_transferred) {
                if (ec) {
                    close();
                    return;
                }
                process_incoming_data(bytes_transferred);
                if (state_ != State::Closed) {
                    do_read();
                }
            }
        );
    }

    void process_incoming_data(size_t bytes_transferred) {
        rx_stream_.insert(rx_stream_.end(), rx_temp_buffer_.data(), rx_temp_buffer_.data() + bytes_transferred);

        size_t processed_offset = 0;
        while (processed_offset < rx_stream_.size()) {
            std::span<const std::byte> remaining(rx_stream_.data() + processed_offset, rx_stream_.size() - processed_offset);
            std::span<const std::byte> cursor = remaining;
            auto length_opt = read_varint(cursor);
            if (!length_opt) break; // Incomplete VarInt prefix

            int32_t packet_len = *length_opt;
            if (packet_len < 0 || packet_len > 2 * 1024 * 1024) {
                std::printf("\033[1;31m[ERROR]\033[0m Invalid packet length: %d\n", packet_len);
                close();
                return;
            }

            size_t length_prefix_len = cursor.data() - remaining.data();
            size_t total_packet_bytes = length_prefix_len + static_cast<size_t>(packet_len);
            if (remaining.size() < total_packet_bytes) {
                break; // Incomplete packet payload, wait for more data
            }

            std::span<const std::byte> packet_body(cursor.data(), packet_len);
            std::span<const std::byte> body_cursor = packet_body;
            auto id_opt = read_varint(body_cursor);
            if (id_opt) {
                int32_t packet_id = *id_opt;
                if (!handle_packet(packet_id, body_cursor)) {
                    close();
                    return;
                }
            }

            processed_offset += total_packet_bytes;
        }

        if (processed_offset > 0) {
            rx_stream_.erase(rx_stream_.begin(), rx_stream_.begin() + processed_offset);
        }
    }

    bool handle_packet(int32_t packet_id, std::span<const std::byte> payload) {
        switch (state_) {
            case State::Handshake: {
                if (packet_id == 0x00) {
                    auto proto_opt = read_varint(payload);
                    auto host_opt  = read_string(payload);
                    auto port_opt  = read_ushort(payload);
                    auto next_opt  = read_varint(payload);
                    if (!proto_opt || !host_opt || !port_opt || !next_opt) {
                        std::printf("\033[1;31m[ERROR]\033[0m Malformed Handshake packet\n");
                        return false;
                    }
                    int32_t next_state = *next_opt;
                    if (next_state == 1) {
                        state_ = State::Status;
                        return true;
                    } else if (next_state == 2) {
                        state_ = State::Login;
                        return true;
                    }
                }
                return false;
            }

            case State::Status: {
                if (packet_id == 0x00) { // Status Request
                    std::printf("\033[1;36m[STATUS]\033[0m Status Request -> Sending Response (%s)\n",
                                target_checker_->is_online() ? "Online" : "Offline");
                    const uint8_t* prefix_ptr = target_checker_->is_online() ?
                        waiting_server::STATUS_PREFIX_ONLINE : waiting_server::STATUS_PREFIX_OFFLINE;
                    size_t prefix_len = target_checker_->is_online() ?
                        waiting_server::STATUS_PREFIX_ONLINE_SIZE : waiting_server::STATUS_PREFIX_OFFLINE_SIZE;
                    const uint8_t* suffix_ptr = target_checker_->is_online() ?
                        waiting_server::STATUS_SUFFIX_ONLINE : waiting_server::STATUS_SUFFIX_OFFLINE;
                    size_t suffix_len = target_checker_->is_online() ?
                        waiting_server::STATUS_SUFFIX_ONLINE_SIZE : waiting_server::STATUS_SUFFIX_OFFLINE_SIZE;

                    const char* motd = "❄ WaitingServer ✦ Linux";
                    size_t motd_len = std::strlen(motd);
                    size_t json_len = prefix_len + motd_len + suffix_len;
                    size_t json_varint_len = waiting_server::varint_size(static_cast<int32_t>(json_len));
                    size_t payload_len = 1 /* packet_id 0x00 */ + json_varint_len + json_len;

                    std::vector<std::byte> pkt;
                    pkt.resize(waiting_server::varint_size(static_cast<int32_t>(payload_len)) + payload_len);
                    size_t offset = 0;
                    offset += waiting_server::write_varint(std::span<std::byte>(pkt.data() + offset, pkt.size() - offset), static_cast<int32_t>(payload_len));
                    pkt[offset++] = std::byte{0x00};
                    offset += waiting_server::write_varint(std::span<std::byte>(pkt.data() + offset, pkt.size() - offset), static_cast<int32_t>(json_len));
                    std::memcpy(pkt.data() + offset, prefix_ptr, prefix_len);
                    offset += prefix_len;
                    std::memcpy(pkt.data() + offset, motd, motd_len);
                    offset += motd_len;
                    std::memcpy(pkt.data() + offset, suffix_ptr, suffix_len);
                    offset += suffix_len;

                    send_dynamic_packet(pkt.data(), offset);
                    return true;
                } else if (packet_id == 0x01) { // Ping Request
                    auto payload_opt = read_ulong(payload);
                    if (!payload_opt) return false;

                    std::array<std::byte, 64> pong_buf;
                    PacketWriter writer(pong_buf);
                    if (writer.write_varint(0x01) && writer.write_ulong(*payload_opt)) {
                        auto resp = writer.finalize();
                        if (resp) {
                            send_dynamic_packet(resp->data(), resp->size(), true); // Close after Pong
                            return true;
                        }
                    }
                    return false;
                }
                return false;
            }

            case State::Login: {
                if (packet_id == 0x00) { // Login Start
                    auto name_opt = read_string(payload);
                    auto uuid_opt = read_uuid(payload);
                    if (!name_opt || !uuid_opt) return false;

                    player_name_ = std::string(*name_opt);
                    player_uuid_ = *uuid_opt;
                    std::printf("\033[1;32m[LOGIN]\033[0m Player '%s' is joining...\n", player_name_.c_str());

                    std::array<std::byte, 256> resp_buf;
                    PacketWriter writer(resp_buf);
                    if (!writer.write_varint(0x02)) return false;
                    if (!writer.write_uuid(player_uuid_)) return false;
                    if (!writer.write_string(player_name_)) return false;
                    if (!writer.write_varint(0)) return false; // 0 properties
                    std::array<std::byte, 16> dummy_session_id = {};
                    if (!writer.write_uuid(dummy_session_id)) return false;

                    auto resp = writer.finalize();
                    if (resp) {
                        send_dynamic_packet(resp->data(), resp->size(), false);
                        return true;
                    }
                    return false;
                } else if (packet_id == 0x03) { // Login Acknowledged
                    std::printf("\033[1;32m[LOGIN]\033[0m Login Acknowledged by '%s'. Starting Configuration sequence...\n",
                                player_name_.c_str());
                    state_ = State::Configuration;
                    config_packet_index_ = 0;
                    sending_config_queue_ = true;

                    if (!target_checker_->is_online()) {
                        target_checker_->trigger_wol();
                    }
                    target_checker_->trigger_immediate_check();

                    advance_config_queue();
                    return true;
                }
                return false;
            }

            case State::Configuration: {
                if (packet_id == 0x00 || packet_id == 0x07 || packet_id == 0x04) {
                    return true;
                } else if (packet_id == 0x03) { // Finish Configuration ACK
                    std::printf("\033[1;34m[CONFIG]\033[0m Finish Configuration ACK from '%s'! Transitioning to PLAY!\n",
                                player_name_.c_str());
                    state_ = State::Play;
                    play_packet_index_ = 0;
                    sending_play_queue_ = true;
                    advance_play_queue();
                    return true;
                }
                return true;
            }

            case State::Play: {
                if (packet_id == 0x00 || packet_id == 0x08 || packet_id == 0x0C || packet_id == 0x1C) {
                    return true;
                }
                return true;
            }

            default:
                return false;
        }
    }

    void advance_config_queue() {
        if (!sending_config_queue_) return;
        if (config_packet_index_ < waiting_server::NUM_CONFIG_PACKETS) {
            auto blob = waiting_server::CONFIG_PACKETS[config_packet_index_++];
            send_static_packet(blob.data, blob.size, false);
        } else {
            sending_config_queue_ = false;
            std::printf("\033[1;34m[CONFIG]\033[0m Finished sending all configuration packets to '%s'. Awaiting FinishConfig ACK...\n",
                        player_name_.c_str());
        }
    }

    void advance_play_queue() {
        if (!sending_play_queue_) return;

        if (play_packet_index_ < waiting_server::NUM_PLAY_PACKETS) {
            if (play_packet_index_ == 0) { // Login (Play) - ID 0x31 (Dynamic entity ID in bytes 2..5)
                int32_t entity_id = 300 + static_cast<int32_t>(session_id_);
                std::vector<uint8_t> pkt(sizeof(waiting_server::play_packet_000));
                std::memcpy(pkt.data(), waiting_server::play_packet_000, sizeof(waiting_server::play_packet_000));
                pkt[2] = static_cast<uint8_t>((entity_id >> 24) & 0xFF);
                pkt[3] = static_cast<uint8_t>((entity_id >> 16) & 0xFF);
                pkt[4] = static_cast<uint8_t>((entity_id >> 8) & 0xFF);
                pkt[5] = static_cast<uint8_t>(entity_id & 0xFF);

                play_packet_index_++;
                send_dynamic_packet(reinterpret_cast<const std::byte*>(pkt.data()), pkt.size(), false);
            } else if (play_packet_index_ == 5) { // Entity Event (Op Permission Level)
                int32_t entity_id = 300 + static_cast<int32_t>(session_id_);
                std::vector<uint8_t> pkt(sizeof(waiting_server::play_packet_005));
                std::memcpy(pkt.data(), waiting_server::play_packet_005, sizeof(waiting_server::play_packet_005));
                pkt[2] = static_cast<uint8_t>((entity_id >> 24) & 0xFF);
                pkt[3] = static_cast<uint8_t>((entity_id >> 16) & 0xFF);
                pkt[4] = static_cast<uint8_t>((entity_id >> 8) & 0xFF);
                pkt[5] = static_cast<uint8_t>(entity_id & 0xFF);

                play_packet_index_++;
                send_dynamic_packet(reinterpret_cast<const std::byte*>(pkt.data()), pkt.size(), false);
            } else if (play_packet_index_ == 9) { // Synchronize Player Position (ID 0x48)
                std::vector<uint8_t> pkt(sizeof(waiting_server::play_packet_009));
                std::memcpy(pkt.data(), waiting_server::play_packet_009, sizeof(waiting_server::play_packet_009));

                auto write_be_double = [](uint8_t* dst, double val) {
                    uint64_t u;
                    std::memcpy(&u, &val, sizeof(u));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                    // Big-endian native
#else
                    u = __builtin_bswap64(u);
#endif
                    std::memcpy(dst, &u, sizeof(u));
                };

                write_be_double(&pkt[3], waiting_server::LOBBY_SPAWN_X);
                write_be_double(&pkt[11], waiting_server::LOBBY_SPAWN_Y);
                write_be_double(&pkt[19], waiting_server::LOBBY_SPAWN_Z);

                play_packet_index_++;
                send_dynamic_packet(reinterpret_cast<const std::byte*>(pkt.data()), pkt.size(), false);
            } else if (play_packet_index_ == 12) { // Player Info Update (ID 0x46)
                std::array<std::byte, 512> buf;
                PacketWriter writer(buf);
                if (writer.write_varint(0x46) &&
                    writer.write_byte(std::byte{0xFF}) && // action mask (all actions)
                    writer.write_varint(1) &&              // entries count
                    writer.write_uuid(player_uuid_) &&
                    writer.write_string(player_name_) &&
                    writer.write_varint(0) &&              // properties count 0
                    writer.write_bool(false) &&            // chat session
                    writer.write_varint(0) &&              // gamemode 0 = survival
                    writer.write_bool(true) &&             // listed
                    writer.write_varint(0) &&              // latency
                    writer.write_bool(false) &&            // display name
                    writer.write_varint(0) &&              // list order
                    writer.write_bool(false)) {            // show hat
                    auto resp = writer.finalize();
                    if (resp) {
                        play_packet_index_++;
                        send_dynamic_packet(resp->data(), resp->size(), false);
                    } else {
                        close();
                    }
                } else {
                    close();
                }
            } else if (play_packet_index_ == 19) { // Set Chunk Cache Center (ID 0x5E)
                std::array<std::byte, 64> buf;
                PacketWriter writer(buf);
                if (writer.write_varint(0x5E) &&
                    writer.write_varint(waiting_server::LOBBY_CENTER_CHUNK_X) &&
                    writer.write_varint(waiting_server::LOBBY_CENTER_CHUNK_Z)) {
                    auto resp = writer.finalize();
                    if (resp) {
                        play_packet_index_++;
                        send_dynamic_packet(resp->data(), resp->size(), false);
                    } else {
                        close();
                    }
                } else {
                    close();
                }
            } else {
                auto blob = waiting_server::PLAY_PACKETS[play_packet_index_++];
                send_static_packet(blob.data, blob.size, false);
            }
        } else if (play_packet_index_ == waiting_server::NUM_PLAY_PACKETS) {
            // Send Entity Metadata (0x63) to show player skin layers and finalize spawn
            std::array<std::byte, 64> buf;
            PacketWriter writer(buf);
            int32_t entity_id = 300 + static_cast<int32_t>(session_id_);
            if (writer.write_varint(0x63) &&
                writer.write_varint(entity_id) &&
                writer.write_byte(std::byte{16}) && // Index 16: skin display mask
                writer.write_varint(0) &&           // Type 0: Byte
                writer.write_byte(std::byte{0x7F}) && // Value: all layers enabled
                writer.write_byte(std::byte{0xFF})) { // End of metadata
                auto resp = writer.finalize();
                if (resp) {
                    play_packet_index_++;
                    send_dynamic_packet(resp->data(), resp->size(), false);
                } else {
                    close();
                }
            } else {
                close();
            }
        } else {
            sending_play_queue_ = false;
            std::printf("\033[1;32m[PLAY]\033[0m Player '%s' spawned in lobby world! (X=%.1f, Y=%.1f, Z=%.1f)\n",
                        player_name_.c_str(), waiting_server::LOBBY_SPAWN_X, waiting_server::LOBBY_SPAWN_Y, waiting_server::LOBBY_SPAWN_Z);

            if (target_checker_->is_online()) {
                send_play_transfer();
            } else {
                target_checker_->trigger_wol();
                start_holding_timers();
            }
        }
    }

    void start_holding_timers() {
        schedule_keepalive();
        schedule_actionbar();
    }

    void schedule_keepalive() {
        keepalive_timer_.expires_after(std::chrono::seconds(5));
        auto self = shared_from_this();
        keepalive_timer_.async_wait([this, self](const std::error_code& ec) {
            if (!ec && state_ == State::Play && !is_transferring_) {
                uint64_t now_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                std::array<std::byte, 64> buf;
                PacketWriter writer(buf);
                if (writer.write_varint(0x2C) && writer.write_ulong(now_ms)) {
                    auto resp = writer.finalize();
                    if (resp) {
                        send_dynamic_packet(resp->data(), resp->size(), false);
                    }
                }
                schedule_keepalive();
            }
        });
    }

    void schedule_actionbar() {
        actionbar_timer_.expires_after(std::chrono::seconds(2));
        auto self = shared_from_this();
        actionbar_timer_.async_wait([this, self](const std::error_code& ec) {
            if (!ec && state_ == State::Play && !is_transferring_) {
                if (target_checker_->is_online()) {
                    send_play_transfer();
                    return;
                }

                std::string_view text = "§c◌ Main server is starting...";
                std::array<std::byte, 256> buf;
                PacketWriter writer(buf);
                if (writer.write_varint(0x57) &&
                    writer.write_byte(std::byte{0x08}) &&
                    writer.write_ushort(static_cast<uint16_t>(text.size()))) {
                    for (char c : text) {
                        writer.write_byte(static_cast<std::byte>(c));
                    }
                    auto resp = writer.finalize();
                    if (resp) {
                        send_dynamic_packet(resp->data(), resp->size(), false);
                    }
                }
                schedule_actionbar();
            }
        });
    }

    void send_play_transfer() {
        if (is_transferring_) return;
        is_transferring_ = true;
        keepalive_timer_.cancel();
        actionbar_timer_.cancel();

        std::printf("\033[1;32m[TRANSFER]\033[0m Primary server ONLINE! Transferring '%s' to %s:%u\n",
                    player_name_.c_str(), config_.target_host.c_str(), config_.target_port);

        std::array<std::byte, 256> buf;
        PacketWriter writer(buf);
        if (writer.write_varint(0x81) &&
            writer.write_string(config_.target_host) &&
            writer.write_varint(config_.target_port)) {
            auto resp = writer.finalize();
            if (resp) {
                send_dynamic_packet(resp->data(), resp->size(), true); // Close after transfer packet
            } else {
                close();
            }
        } else {
            close();
        }
    }

    void send_dynamic_packet(const std::byte* data, size_t size, bool close_after = false) {
        OutboundItem item;
        item.dynamic_buffer.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
        item.close_after = close_after;
        outbound_queue_.push_back(std::move(item));
        if (!is_writing_) {
            do_write();
        }
    }

    void send_static_packet(const uint8_t* data, size_t size, bool close_after = false) {
        OutboundItem item;
        item.static_ptr = data;
        item.static_size = size;
        item.close_after = close_after;
        outbound_queue_.push_back(std::move(item));
        if (!is_writing_) {
            do_write();
        }
    }

    void do_write() {
        if (outbound_queue_.empty()) {
            is_writing_ = false;
            if (sending_config_queue_) {
                advance_config_queue();
            } else if (sending_play_queue_) {
                advance_play_queue();
            }
            return;
        }

        is_writing_ = true;
        auto& item = outbound_queue_.front();
        asio::const_buffer buf = item.static_ptr
            ? asio::const_buffer(item.static_ptr, item.static_size)
            : asio::const_buffer(item.dynamic_buffer.data(), item.dynamic_buffer.size());

        bool close_after = item.close_after;
        auto self = shared_from_this();

        asio::async_write(
            socket_, buf,
            [this, self, close_after](const std::error_code& ec, size_t) {
                if (ec) {
                    close();
                    return;
                }
                outbound_queue_.pop_front();
                if (close_after) {
                    close();
                    return;
                }
                do_write();
            }
        );
    }

    asio::ip::tcp::socket socket_;
    MinecraftServer& server_;
    const Config& config_;
    std::shared_ptr<TargetChecker> target_checker_;
    uint32_t session_id_;
    State state_;

    std::array<std::byte, 4096> rx_temp_buffer_ = {};
    std::vector<std::byte> rx_stream_;

    std::deque<OutboundItem> outbound_queue_;
    bool is_writing_ = false;

    std::string player_name_;
    std::array<std::byte, 16> player_uuid_ = {};

    size_t config_packet_index_ = 0;
    size_t play_packet_index_ = 0;
    bool sending_config_queue_ = false;
    bool sending_play_queue_ = false;
    bool is_transferring_ = false;

    asio::steady_timer keepalive_timer_;
    asio::steady_timer actionbar_timer_;
};

class MinecraftServer {
public:
    MinecraftServer(asio::io_context& io_ctx, const Config& config, std::shared_ptr<TargetChecker> checker)
        : io_ctx_(io_ctx)
        , config_(config)
        , target_checker_(std::move(checker))
        , acceptor_(io_ctx)
        , next_session_id_(1)
    {
        auto address = asio::ip::make_address(config_.listen_address);
        asio::ip::tcp::endpoint endpoint(address, config_.listen_port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();

        std::printf("\033[1;32m[SERVER]\033[0m WaitingServer listening on %s:%u\n",
                    config_.listen_address.c_str(), config_.listen_port);

        target_checker_->set_status_callback([this](bool online) {
            if (online) {
                // Prune dead sessions and notify active play sessions
                cleanup_sessions();
                for (auto& session : sessions_) {
                    session->on_target_online();
                }
            }
        });

        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](const std::error_code& ec, asio::ip::tcp::socket socket) {
                if (!ec) {
                    cleanup_sessions();
                    auto session = std::make_shared<ClientSession>(
                        std::move(socket), *this, config_, target_checker_, next_session_id_++
                    );
                    sessions_.push_back(session);
                    session->start();
                }
                do_accept();
            }
        );
    }

    void cleanup_sessions() {
        // Remove sessions where socket is closed
        sessions_.erase(
            std::remove_if(sessions_.begin(), sessions_.end(),
                           [](const std::shared_ptr<ClientSession>& s) {
                               return s.use_count() <= 1;
                           }),
            sessions_.end());
    }

    asio::io_context& io_ctx_;
    const Config& config_;
    std::shared_ptr<TargetChecker> target_checker_;
    asio::ip::tcp::acceptor acceptor_;
    uint32_t next_session_id_;
    std::vector<std::shared_ptr<ClientSession>> sessions_;
};

} // namespace waiting_server

int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string_view arg1 = argv[1];
        if (arg1 == "--version" || arg1 == "-v") {
            std::printf("WaitingServer Linux (Minecraft Java Edition Protocol 777 / 26.3)\n");
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h") {
            std::printf("Usage: waiting_server [config_path]\n");
            std::printf("Options:\n");
            std::printf("  -h, --help     Show this help message\n");
            std::printf("  -v, --version  Show version information\n");
            return 0;
        }
    }

    std::string config_path = "config.txt";
    if (argc > 1) {
        config_path = argv[1];
    }

    waiting_server::Config config;
    if (!waiting_server::load_config(config_path, config)) {
        std::printf("\033[1;33m[WARN]\033[0m Could not open '%s', using defaults.\n", config_path.c_str());
    } else {
        std::printf("\033[1;32m[CONFIG]\033[0m Loaded configuration from '%s'\n", config_path.c_str());
    }

    std::printf("\033[1;36m[CONFIG]\033[0m Target: %s:%u (MAC: %s)\n",
                config.target_host.c_str(), config.target_port, config.target_mac.c_str());
    std::printf("\033[1;36m[CONFIG]\033[0m Listen: %s:%u\n",
                config.listen_address.c_str(), config.listen_port);

    try {
        asio::io_context io_ctx;

        asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
        signals.async_wait([&](const std::error_code&, int) {
            std::printf("\n\033[1;33m[SERVER]\033[0m Received termination signal. Shutting down...\n");
            io_ctx.stop();
        });

        auto target_checker = std::make_shared<waiting_server::TargetChecker>(io_ctx, config);
        target_checker->start();

        waiting_server::MinecraftServer server(io_ctx, config, target_checker);

        io_ctx.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\033[1;31m[FATAL ERROR]\033[0m %s\n", e.what());
        return 1;
    }

    return 0;
}
