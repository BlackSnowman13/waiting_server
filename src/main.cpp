#include <asio.hpp>
#include "packet_reader.hpp"
#include "packet_writer.hpp"
#include <array>
#include <span>
#include <optional>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace waiting_server {

// --- Configuration Structure ---
struct ServerConfig {
    std::array<char, 128> target_host = {};
    uint16_t target_port = 25565;
    std::array<char, 64> target_mac = {};
};

// --- Player Sample Cache Entry ---
struct PlayerSampleEntry {
    std::array<char, 32> name = {};
    std::array<char, 37> id = {}; // UUID string
};

// --- Global Cached Server State ---
struct GlobalServerState {
    bool online = false;
    int32_t online_players = 0;
    int32_t max_players = 20;
    std::array<PlayerSampleEntry, 32> player_sample = {};
    size_t player_sample_count = 0;
    
    std::array<char, 4096> json_response = {};
    size_t json_response_len = 0;
};

// --- Time and Formatting Logging Helpers ---
inline void log_time() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto time_t_now = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    
    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);
    
    std::printf("[%02d:%02d:%02d.%03d] ", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, static_cast<int>(ms.count()));
}

template<typename... Args>
void log_info(const char* format, Args... args) {
    log_time();
    std::printf("\033[1;32m[INFO]\033[0m ");
    if constexpr (sizeof...(args) == 0) {
        std::printf("%s", format);
    } else {
        std::printf(format, args...);
    }
    std::printf("\n");
}

template<typename... Args>
void log_error(const char* format, Args... args) {
    log_time();
    std::printf("\033[1;31m[ERROR]\033[0m ");
    if constexpr (sizeof...(args) == 0) {
        std::printf("%s", format);
    } else {
        std::printf(format, args...);
    }
    std::printf("\n");
}

// --- Configuration File Reader ---
inline bool load_config(const char* path, ServerConfig& config) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(key);
        trim(val);
        
        if (key == "target_host") {
            std::strncpy(config.target_host.data(), val.c_str(), config.target_host.size() - 1);
        } else if (key == "target_port") {
            config.target_port = static_cast<uint16_t>(std::stoi(val));
        } else if (key == "target_mac") {
            std::strncpy(config.target_mac.data(), val.c_str(), config.target_mac.size() - 1);
        }
    }
    return true;
}

inline void create_default_config(const char* path) {
    std::ofstream file(path);
    if (file.is_open()) {
        file << "# Waiting Server Configuration\n"
             << "target_host=64-pinned-potato-actual\n"
             << "target_port=25565\n"
             << "target_mac=AA:BB:CC:DD:EE:FF\n";
    }
}

// --- Light-weight, Zero-allocation JSON Scanner ---
inline std::optional<int32_t> find_json_int(std::string_view json, std::string_view key) {
    char search_pattern[64];
    std::snprintf(search_pattern, sizeof(search_pattern), "\"%.*s\":", static_cast<int>(key.size()), key.data());
    
    auto pos = json.find(search_pattern);
    if (pos == std::string::npos) {
        std::snprintf(search_pattern, sizeof(search_pattern), "\"%.*s\" :", static_cast<int>(key.size()), key.data());
        pos = json.find(search_pattern);
        if (pos == std::string::npos) return std::nullopt;
    }
    
    pos = json.find(':', pos);
    if (pos == std::string::npos) return std::nullopt;
    pos++;
    
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    
    if (pos >= json.size()) return std::nullopt;
    
    int32_t val = 0;
    bool found = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
        found = true;
    }
    
    if (!found) return std::nullopt;
    return val;
}

inline size_t parse_player_sample(std::string_view json, std::array<PlayerSampleEntry, 32>& sample_arr) {
    auto sample_pos = json.find("\"sample\"");
    if (sample_pos == std::string::npos) return 0;
    
    auto start = json.find('[', sample_pos);
    if (start == std::string::npos) return 0;
    
    auto end = json.find(']', start);
    if (end == std::string::npos) return 0;
    
    std::string_view sample_data = json.substr(start, end - start + 1);
    
    size_t count = 0;
    size_t pos = 0;
    while (count < 32) {
        // Find the next object start '{'
        auto obj_start = sample_data.find('{', pos);
        if (obj_start == std::string::npos) break;
        
        auto obj_end = sample_data.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        
        std::string_view obj = sample_data.substr(obj_start, obj_end - obj_start + 1);
        
        // Find "name" and "id" inside this object (order does not matter!)
        auto name_key = obj.find("\"name\"");
        auto id_key = obj.find("\"id\"");
        
        if (name_key == std::string::npos || id_key == std::string::npos) {
            pos = obj_end + 1;
            continue;
        }
        
        // Parse name value
        auto name_val_start = obj.find('\"', name_key + 6);
        if (name_val_start == std::string::npos) { pos = obj_end + 1; continue; }
        name_val_start++;
        auto name_val_end = obj.find('\"', name_val_start);
        if (name_val_end == std::string::npos) { pos = obj_end + 1; continue; }
        
        // Parse id value
        auto id_val_start = obj.find('\"', id_key + 4);
        if (id_val_start == std::string::npos) { pos = obj_end + 1; continue; }
        id_val_start++;
        auto id_val_end = obj.find('\"', id_val_start);
        if (id_val_end == std::string::npos) { pos = obj_end + 1; continue; }
        
        std::string_view name_sv = obj.substr(name_val_start, name_val_end - name_val_start);
        std::string_view id_sv = obj.substr(id_val_start, id_val_end - id_val_start);
        
        auto& entry = sample_arr[count];
        std::strncpy(entry.name.data(), name_sv.data(), std::min(name_sv.size(), entry.name.size() - 1));
        entry.name[std::min(name_sv.size(), entry.name.size() - 1)] = '\0';
        
        std::strncpy(entry.id.data(), id_sv.data(), std::min(id_sv.size(), entry.id.size() - 1));
        entry.id[std::min(id_sv.size(), entry.id.size() - 1)] = '\0';
        
        count++;
        pos = obj_end + 1;
    }
    return count;
}

inline void format_global_json(GlobalServerState& state) {
    char sample_buf[3072] = {};
    size_t sample_len = 0;
    
    if (state.online && state.player_sample_count > 0) {
        sample_len = std::snprintf(sample_buf, sizeof(sample_buf), "[");
        for (size_t i = 0; i < state.player_sample_count; ++i) {
            char entry_buf[128];
            int written = std::snprintf(entry_buf, sizeof(entry_buf),
                "{\"name\":\"%s\",\"id\":\"%s\"}%s",
                state.player_sample[i].name.data(),
                state.player_sample[i].id.data(),
                (i + 1 < state.player_sample_count) ? "," : ""
            );
            if (sample_len + written < sizeof(sample_buf)) {
                std::strncat(sample_buf, entry_buf, sizeof(sample_buf) - sample_len - 1);
                sample_len += written;
            }
        }
        if (sample_len + 1 < sizeof(sample_buf)) {
            std::strncat(sample_buf, "]", sizeof(sample_buf) - sample_len - 1);
            sample_len++;
        }
    } else {
        std::strcpy(sample_buf, "[]");
    }
    
    const char* status_name = state.online ? "● Online" : "◌ Offline";
    int32_t status_proto = state.online ? 775 : 6767;
    const char* online_text = state.online ? "● Main server is online" : "◌ Main server is offline";
    const char* online_color = state.online ? "green" : "red";
    
    char players_str[32];
    std::snprintf(players_str, sizeof(players_str), "%d/%d", state.online_players, state.max_players);
    
    int written = std::snprintf(
        state.json_response.data(), state.json_response.size(),
        "{"
          "\"version\":{\"name\":\"%s\",\"protocol\":%d},"
          "\"players\":{\"max\":%d,\"online\":%d,\"sample\":%s},"
          "\"description\":{"
            "\"text\":\"\","
            "\"extra\":["
              "{\"text\":\"\\u2744 \",\"color\":\"aqua\"},"
              "{\"text\":\"SnowGlobe\",\"color\":\"white\",\"bold\":true},"
              "{\"text\":\"  \\u2726  \",\"color\":\"dark_gray\"},"
              "{\"text\":\"v3\",\"color\":\"gray\"},"
              "{\"text\":\"\\n\"},"
              "{\"text\":\"%s\",\"color\":\"%s\"},"
              "{\"text\":\"\\n\"},"
              "{\"text\":\"\\u2603 Snowmen online: \",\"color\":\"aqua\"},"
              "{\"text\":\"%s\",\"color\":\"white\",\"bold\":true}"
            "]"
          "},"
          "\"enforcesSecureChat\":false"
        "}",
        status_name, status_proto,
        state.max_players, state.online_players, sample_buf,
        online_text, online_color,
        players_str
    );
    
    state.json_response_len = (written > 0) ? static_cast<size_t>(written) : 0;
}

// --- Connection Structure ---
struct Connection {
    size_t index = 0;
    asio::ip::tcp::socket socket;
    
    // In-place static buffers to ensure zero runtime heap allocation
    std::array<std::byte, 4096> rx_buffer;
    size_t rx_len = 0;
    
    std::array<std::byte, 4096> tx_buffer;
    
    // Pre-allocated socket to test target server status
    asio::ip::tcp::socket check_socket;
    
    // Pre-allocated timer for keep-alive packets and waiting loops
    asio::steady_timer keepalive_timer;
    
    // Pre-allocated timer for checking target server connection timeout
    asio::steady_timer connect_timer;
    
    enum class State {
        Handshake,
        Status,
        Login,
        Configuration,
        Closed
    } state = State::Handshake;
    
    bool is_active = false;
    
    std::array<char, 64> remote_ip = {};
    uint16_t remote_port = 0;
    
    // Cache player credentials to use in logs
    std::array<char, 32> player_name = {};
    
    Connection(asio::io_context& io, size_t idx) 
        : index(idx), socket(io), check_socket(io), keepalive_timer(io), connect_timer(io) {
        reset();
    }
    
    void reset() {
        state = State::Handshake;
        rx_len = 0;
        is_active = false;
        remote_ip.fill('\0');
        remote_port = 0;
        player_name.fill('\0');
        
        std::error_code ec;
        keepalive_timer.cancel(ec);
        connect_timer.cancel(ec);
        check_socket.close(ec);
    }
};

// --- Core Server Class ---
class Server {
public:
    Server(asio::io_context& io_context, uint16_t port, const ServerConfig& config)
        : acceptor_(io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
          dummy_socket_(io_context),
          resolver_(io_context),
          config_(config),
          poll_socket_(io_context),
          poll_timer_(io_context) {
        
        // Initialize global state with default values
        global_state_.online = false;
        global_state_.online_players = 0;
        global_state_.max_players = 20;
        global_state_.player_sample_count = 0;
        format_global_json(global_state_);
        
        for (size_t i = 0; i < connection_pool_.size(); ++i) {
            connection_pool_[i].emplace(io_context, i);
        }
        
        start_accept();
        
        // Trigger immediate status poll
        schedule_poll(0);
    }

private:
    void start_accept() {
        Connection* free_conn = nullptr;
        for (auto& conn_opt : connection_pool_) {
            if (conn_opt && !conn_opt->is_active) {
                free_conn = &(*conn_opt);
                break;
            }
        }
        
        if (!free_conn) {
            acceptor_.async_accept(dummy_socket_, [this](std::error_code ec) {
                if (!ec) {
                    std::error_code close_ec;
                    dummy_socket_.close(close_ec);
                }
                start_accept();
            });
        } else {
            acceptor_.async_accept(free_conn->socket, [this, free_conn](std::error_code ec) {
                if (!ec) {
                    handle_accept(free_conn);
                }
                start_accept();
            });
        }
    }
    
    void handle_accept(Connection* conn) {
        conn->is_active = true;
        std::error_code ec;
        auto ep = conn->socket.remote_endpoint(ec);
        if (!ec) {
            auto addr_str = ep.address().to_string(ec);
            if (!ec) {
                std::strncpy(conn->remote_ip.data(), addr_str.c_str(), conn->remote_ip.size() - 1);
            }
            conn->remote_port = ep.port();
        }
                 
        start_read(conn);
    }
    
    void start_read(Connection* conn) {
        if (!conn->is_active) return;
        
        conn->socket.async_read_some(
            asio::buffer(conn->rx_buffer.data() + conn->rx_len, conn->rx_buffer.size() - conn->rx_len),
            [this, conn](std::error_code ec, size_t bytes_transferred) {
                if (!conn->is_active) {
                    return;
                }
                if (ec) {
                    if (conn->state == Connection::State::Configuration) {
                        log_info("[%s] Player disconnected.", conn->player_name.data());
                    }
                    close_connection(conn);
                    return;
                }
                
                conn->rx_len += bytes_transferred;
                process_packets(conn);
            }
        );
    }
    
    void process_packets(Connection* conn) {
        while (conn->is_active) {
            std::span<const std::byte> rx_span(conn->rx_buffer.data(), conn->rx_len);
            if (rx_span.empty()) {
                break;
            }
            
            std::span<const std::byte> temp_span = rx_span;
            auto length_opt = read_varint(temp_span);
            if (!length_opt) {
                break; 
            }
            
            int32_t length = *length_opt;
            if (length <= 0 || length > 32768) {
                close_connection(conn);
                return;
            }
            
            size_t length_varint_bytes = rx_span.size() - temp_span.size();
            size_t total_packet_bytes = length_varint_bytes + static_cast<size_t>(length);
            
            if (rx_span.size() < total_packet_bytes) {
                break; 
            }
            
            std::span<const std::byte> packet_span = rx_span.subspan(length_varint_bytes, length);
            
            bool success = handle_packet(conn, packet_span);
            
            consume_rx_bytes(conn, total_packet_bytes);
            
            if (!success) {
                close_connection(conn);
                return;
            }
        }
        
        if (conn->is_active) {
            if (conn->rx_len >= conn->rx_buffer.size()) {
                close_connection(conn);
                return;
            }
            start_read(conn);
        }
    }
    
    void consume_rx_bytes(Connection* conn, size_t count) {
        if (count >= conn->rx_len) {
            conn->rx_len = 0;
        } else {
            std::memmove(conn->rx_buffer.data(), conn->rx_buffer.data() + count, conn->rx_len - count);
            conn->rx_len -= count;
        }
    }
    
    bool handle_packet(Connection* conn, std::span<const std::byte> packet_data) {
        PacketReader reader(packet_data);
        auto packet_id_opt = reader.read_varint();
        if (!packet_id_opt) {
            return false;
        }
        
        int32_t packet_id = *packet_id_opt;
        
        switch (conn->state) {
            case Connection::State::Handshake: {
                if (packet_id != 0x00) {
                    return false;
                }
                
                auto proto_version_opt = reader.read_varint();
                auto server_addr_opt = reader.read_string();
                auto server_port_opt = reader.read_ushort();
                auto next_state_opt = reader.read_varint();
                
                if (!proto_version_opt || !server_addr_opt || !server_port_opt || !next_state_opt) {
                    return false;
                }
                
                int32_t next_state = *next_state_opt;
                if (next_state == 1) {
                    conn->state = Connection::State::Status;
                } else if (next_state == 2) {
                    conn->state = Connection::State::Login;
                } else {
                    return false;
                }
                return true;
            }
            case Connection::State::Status: {
                if (packet_id == 0x00) {
                    log_info("[%s] Pinged server list (Status State)", conn->remote_ip.data());
                    
                    PacketWriter writer(conn->tx_buffer);
                    if (!writer.write_varint(0x00)) return false; 
                    
                    std::string_view resp_sv(global_state_.json_response.data(), global_state_.json_response_len);
                    if (!writer.write_string(resp_sv)) return false;
                    
                    auto resp = writer.finalize();
                    if (!resp) return false;
                    
                    write_response(conn, *resp, false); 
                    return true;
                } else if (packet_id == 0x01) {
                    auto payload_opt = reader.read_ulong();
                    if (!payload_opt) return false;
                    uint64_t payload = *payload_opt;
                    
                    PacketWriter writer(conn->tx_buffer);
                    if (!writer.write_varint(0x01)) return false; 
                    if (!writer.write_ulong(payload)) return false;
                    
                    auto resp = writer.finalize();
                    if (!resp) return false;
                    
                    write_response(conn, *resp, true); 
                    return true;
                } else {
                    return false;
                }
            }
            case Connection::State::Login: {
                if (packet_id == 0x00) {
                    auto name_opt = reader.read_string();
                    auto uuid_opt = reader.read_uuid();
                    
                    if (!name_opt || !uuid_opt) return false;
                    
                    std::string_view name = *name_opt;
                    std::array<std::byte, 16> uuid = *uuid_opt;
                    
                    std::strncpy(conn->player_name.data(), name.data(), std::min(name.size(), conn->player_name.size() - 1));
                    
                    char uuid_str[37] = {};
                    auto to_hex = [](std::byte b, char* dest) {
                        static constexpr char hex_digits[] = "0123456789abcdef";
                        uint8_t val = static_cast<uint8_t>(b);
                        dest[0] = hex_digits[val >> 4];
                        dest[1] = hex_digits[val & 0x0F];
                    };
                    to_hex(uuid[0], &uuid_str[0]);
                    to_hex(uuid[1], &uuid_str[2]);
                    to_hex(uuid[2], &uuid_str[4]);
                    to_hex(uuid[3], &uuid_str[6]);
                    uuid_str[8] = '-';
                    to_hex(uuid[4], &uuid_str[9]);
                    to_hex(uuid[5], &uuid_str[11]);
                    uuid_str[13] = '-';
                    to_hex(uuid[6], &uuid_str[14]);
                    to_hex(uuid[7], &uuid_str[16]);
                    uuid_str[18] = '-';
                    to_hex(uuid[8], &uuid_str[19]);
                    to_hex(uuid[9], &uuid_str[21]);
                    uuid_str[23] = '-';
                    to_hex(uuid[10], &uuid_str[24]);
                    to_hex(uuid[11], &uuid_str[26]);
                    to_hex(uuid[12], &uuid_str[28]);
                    to_hex(uuid[13], &uuid_str[30]);
                    to_hex(uuid[14], &uuid_str[32]);
                    to_hex(uuid[15], &uuid_str[34]);
                    uuid_str[36] = '\0';
                    
                    log_info("[%s] Player '%s' (UUID: %s) is joining.", 
                             conn->remote_ip.data(), conn->player_name.data(), uuid_str);
                             
                    PacketWriter writer(conn->tx_buffer);
                    if (!writer.write_varint(0x02)) return false; 
                    if (!writer.write_uuid(uuid)) return false;
                    if (!writer.write_string(name)) return false;
                    if (!writer.write_varint(0)) return false; 
                    
                    auto resp = writer.finalize();
                    if (!resp) return false;
                    
                    write_response(conn, *resp, false); 
                    return true;
                } else if (packet_id == 0x03) {
                    conn->state = Connection::State::Configuration;
                    
                    check_target_server(conn);
                    return true;
                } else {
                    return false;
                }
            }
            case Connection::State::Configuration: {
                if (packet_id == 0x00) {
                    return true;
                } else if (packet_id == 0x04) {
                    return true;
                } else {
                    return true; 
                }
            }
            default:
                return false;
        }
    }
    
    // --- Target Server Check & WOL Engine ---
    void check_target_server(Connection* conn) {
        std::error_code close_ec;
        conn->check_socket.close(close_ec);
        
        resolver_.async_resolve(
            config_.target_host.data(), std::to_string(config_.target_port),
            [this, conn](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
                if (ec || results.empty()) {
                    log_error("[%s] DNS resolution failed for target server: %s", conn->player_name.data(), ec.message().c_str());
                    handle_target_offline(conn);
                    return;
                }
                
                // Start connection timeout timer for 1.5 seconds
                conn->connect_timer.expires_after(std::chrono::milliseconds(1500));
                conn->connect_timer.async_wait([conn](std::error_code ec) {
                    if (!ec) {
                        std::error_code close_ec;
                        conn->check_socket.close(close_ec);
                    }
                });
                
                asio::async_connect(
                    conn->check_socket,
                    results,
                    [this, conn](std::error_code ec, const asio::ip::tcp::endpoint& endpoint) {
                        std::error_code cancel_ec;
                        conn->connect_timer.cancel(cancel_ec);
                        
                        std::error_code close_ec;
                        conn->check_socket.close(close_ec);
                        
                        if (ec) {
                            handle_target_offline(conn);
                        } else {
                            log_info("[%s] Target server is ONLINE. Transferring player to %s:%d.", 
                                     conn->player_name.data(), config_.target_host.data(), config_.target_port);
                            send_transfer_packet(conn);
                        }
                    }
                );
            }
        );
    }
    
    void handle_target_offline(Connection* conn) {
        log_info("[%s] Target server is OFFLINE. Triggering Wake-on-LAN for %s. Holding player in lobby.", 
                 conn->player_name.data(), config_.target_mac.data());
                 
        char cmd_buf[128] = {};
        std::snprintf(cmd_buf, sizeof(cmd_buf), "wakeonlan %s &", config_.target_mac.data());
        std::system(cmd_buf);
        
        start_configuration_waiting_loop(conn);
    }
    
    void start_configuration_waiting_loop(Connection* conn) {
        if (!conn->is_active || conn->state != Connection::State::Configuration) return;
        
        conn->keepalive_timer.expires_after(std::chrono::seconds(5));
        conn->keepalive_timer.async_wait([this, conn](std::error_code ec) {
            if (ec) return; 
            
            if (conn->is_active && conn->state == Connection::State::Configuration) {
                send_keepalive_packet(conn);
                recheck_target_server(conn);
            }
        });
    }
    
    void send_keepalive_packet(Connection* conn) {
        uint64_t keepalive_id = std::chrono::steady_clock::now().time_since_epoch().count();
        PacketWriter writer(conn->tx_buffer);
        if (!writer.write_varint(0x04)) return; 
        if (!writer.write_ulong(keepalive_id)) return;
        
        auto resp = writer.finalize();
        if (resp) {
            write_response(conn, *resp, false);
        }
    }
    
    void recheck_target_server(Connection* conn) {
        std::error_code close_ec;
        conn->check_socket.close(close_ec);
        
        resolver_.async_resolve(
            config_.target_host.data(), std::to_string(config_.target_port),
            [this, conn](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
                if (ec || results.empty()) {
                    start_configuration_waiting_loop(conn);
                    return;
                }
                
                // Start connection timeout timer for 1.5 seconds
                conn->connect_timer.expires_after(std::chrono::milliseconds(1500));
                conn->connect_timer.async_wait([conn](std::error_code ec) {
                    if (!ec) {
                        std::error_code close_ec;
                        conn->check_socket.close(close_ec);
                    }
                });
                
                conn->check_socket.async_connect(
                    *results.begin(),
                    [this, conn](std::error_code ec) {
                        std::error_code cancel_ec;
                        conn->connect_timer.cancel(cancel_ec);
                        
                        std::error_code close_ec;
                        conn->check_socket.close(close_ec);
                        
                        if (!ec) {
                            log_info("[%s] Target server has come ONLINE. Transferring player.", conn->player_name.data());
                            send_transfer_packet(conn);
                        } else {
                            start_configuration_waiting_loop(conn);
                        }
                    }
                );
            }
        );
    }
    
    void send_transfer_packet(Connection* conn) {
        PacketWriter writer(conn->tx_buffer);
        if (!writer.write_varint(0x0B)) return; 
        if (!writer.write_string(config_.target_host.data())) return;
        if (!writer.write_varint(config_.target_port)) return; 
        
        auto resp = writer.finalize();
        if (resp) {
            write_response(conn, *resp, true); 
        }
    }
    
    void write_response(Connection* conn, std::span<const std::byte> data, bool close_after) {
        if (!conn->is_active) return;
        
        asio::async_write(
            conn->socket,
            asio::buffer(data.data(), data.size()),
            [this, conn, close_after](std::error_code ec, size_t bytes_transferred) {
                if (!conn->is_active) return;
                
                if (ec) {
                    close_connection(conn);
                    return;
                }
                
                if (close_after) {
                    close_connection(conn);
                }
            }
        );
    }
    
    void close_connection(Connection* conn) {
        if (!conn->is_active) return;
        
        std::error_code ec;
        conn->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        conn->socket.close(ec);
        conn->reset();
    }

    // --- Background Target Server Polling Engine ---
    void schedule_poll(int seconds) {
        poll_timer_.expires_after(std::chrono::seconds(seconds));
        poll_timer_.async_wait([this](std::error_code ec) {
            if (ec) return;
            start_poll_cycle();
        });
    }

    void start_poll_cycle() {
        std::error_code close_ec;
        poll_socket_.close(close_ec);
        
        resolver_.async_resolve(
            config_.target_host.data(), std::to_string(config_.target_port),
            [this](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
                if (ec || results.empty()) {
                    handle_poll_failure();
                    return;
                }
                
                asio::async_connect(
                    poll_socket_,
                    results,
                    [this](std::error_code ec, const asio::ip::tcp::endpoint&) {
                        if (ec) {
                            handle_poll_failure();
                        } else {
                            send_poll_requests();
                        }
                    }
                );
            }
        );
    }

    void send_poll_requests() {
        std::array<std::byte, 256> h_buf;
        PacketWriter h_writer(h_buf);
        h_writer.write_varint(0x00);
        h_writer.write_varint(775);
        h_writer.write_string(config_.target_host.data());
        h_writer.write_ushort(config_.target_port);
        h_writer.write_varint(1); // Status next state
        auto h_span = h_writer.finalize();
        
        std::array<std::byte, 32> r_buf;
        PacketWriter r_writer(r_buf);
        r_writer.write_varint(0x00); // Status request ID
        auto r_span = r_writer.finalize();
        
        if (!h_span || !r_span) {
            handle_poll_failure();
            return;
        }
        
        // Combine packets
        static std::array<std::byte, 300> combined_buf;
        std::memcpy(combined_buf.data(), h_span->data(), h_span->size());
        std::memcpy(combined_buf.data() + h_span->size(), r_span->data(), r_span->size());
        
        asio::async_write(
            poll_socket_,
            asio::buffer(combined_buf.data(), h_span->size() + r_span->size()),
            [this](std::error_code ec, size_t) {
                if (ec) {
                    handle_poll_failure();
                    return;
                }
                poll_rx_len_ = 0;
                start_read_poll_response();
            }
        );
    }

    void start_read_poll_response() {
        poll_socket_.async_read_some(
            asio::buffer(poll_rx_buffer_.data() + poll_rx_len_, poll_rx_buffer_.size() - poll_rx_len_),
            [this](std::error_code ec, size_t bytes) {
                if (ec) {
                    process_poll_response();
                    return;
                }
                poll_rx_len_ += bytes;
                
                // Check if we received full packet payload
                std::span<const std::byte> rx_span(poll_rx_buffer_.data(), poll_rx_len_);
                std::span<const std::byte> temp = rx_span;
                auto len_opt = read_varint(temp);
                if (len_opt) {
                    int32_t len = *len_opt;
                    size_t len_varint_bytes = rx_span.size() - temp.size();
                    if (rx_span.size() >= len_varint_bytes + len) {
                        process_poll_response();
                        return;
                    }
                }
                
                if (poll_rx_len_ >= poll_rx_buffer_.size()) {
                    handle_poll_failure();
                    return;
                }
                start_read_poll_response();
            }
        );
    }

    void process_poll_response() {
        std::error_code close_ec;
        poll_socket_.close(close_ec);
        
        std::span<const std::byte> rx_span(poll_rx_buffer_.data(), poll_rx_len_);
        std::span<const std::byte> temp = rx_span;
        auto len_opt = read_varint(temp);
        if (!len_opt) {
            handle_poll_failure();
            return;
        }
        
        int32_t len = *len_opt;
        size_t len_varint_bytes = rx_span.size() - temp.size();
        if (rx_span.size() < len_varint_bytes + len) {
            handle_poll_failure();
            return;
        }
        
        std::span<const std::byte> payload = rx_span.subspan(len_varint_bytes, len);
        PacketReader reader(payload);
        auto packet_id_opt = reader.read_varint();
        if (!packet_id_opt || *packet_id_opt != 0x00) {
            handle_poll_failure();
            return;
        }
        
        auto json_opt = reader.read_string();
        if (!json_opt) {
            handle_poll_failure();
            return;
        }
        
        std::string_view json = *json_opt;
        
        auto max_opt = find_json_int(json, "max");
        auto online_opt = find_json_int(json, "online");
        
        global_state_.online = true;
        global_state_.max_players = max_opt ? *max_opt : 20;
        global_state_.online_players = online_opt ? *online_opt : 0;
        global_state_.player_sample_count = parse_player_sample(json, global_state_.player_sample);
        
        format_global_json(global_state_);
        schedule_poll(30);
    }

    void handle_poll_failure() {
        std::error_code close_ec;
        poll_socket_.close(close_ec);
        
        global_state_.online = false;
        global_state_.online_players = 0;
        global_state_.player_sample_count = 0;
        
        format_global_json(global_state_);
        schedule_poll(30);
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket dummy_socket_;
    asio::ip::tcp::resolver resolver_;
    ServerConfig config_;
    std::array<std::optional<Connection>, 32> connection_pool_;
    
    // Polling objects
    asio::ip::tcp::socket poll_socket_;
    asio::steady_timer poll_timer_;
    std::array<std::byte, 8192> poll_rx_buffer_;
    size_t poll_rx_len_ = 0;
    GlobalServerState global_state_;
};

} // namespace waiting_server

int main(int argc, char* argv[]) {
    uint16_t port = 25565;
    if (argc > 1) {
        int parsed_port = std::atoi(argv[1]);
        if (parsed_port > 0 && parsed_port <= 65535) {
            port = static_cast<uint16_t>(parsed_port);
        }
    }
    
    waiting_server::ServerConfig config;
    const char* config_path = "config.txt";
    if (!waiting_server::load_config(config_path, config)) {
        waiting_server::log_info("Configuration file config.txt not found. Creating default...");
        waiting_server::create_default_config(config_path);
        waiting_server::load_config(config_path, config);
    }
    
    try {
        asio::io_context io_context(1);
        
        waiting_server::log_info("Starting Waiting Server on port %d...", port);
        waiting_server::log_info("Target host: %s, Target port: %d, Target MAC: %s", 
                                 config.target_host.data(), config.target_port, config.target_mac.data());
        waiting_server::log_info("Memory Model: Zero Runtime Heap Allocation post-startup active.");
        
        waiting_server::Server server(io_context, port, config);
        
        io_context.run();
    } catch (const std::exception& e) {
        waiting_server::log_error("Server exception in main: %s", e.what());
        return 1;
    }
    
    return 0;
}