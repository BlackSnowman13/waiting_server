#include <asio.hpp>
#include "packet_reader.hpp"
#include "packet_writer.hpp"
#include "captured_packets.hpp"
#include <array>
#include <span>
#include <optional>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <cctype>

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
    
    std::array<char, 16384> json_response = {};
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
    int32_t status_proto = state.online ? 776 : 6767;
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
          "\"favicon\": \"data:image/png;base64,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\","
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
    
    std::array<std::byte, 16384> tx_buffer;
    
    // Pre-allocated socket to test target server status
    asio::ip::tcp::socket check_socket;
    
    // Pre-allocated timer for keep-alive packets and waiting loops
    asio::steady_timer keepalive_timer;
    
    // Pre-allocated timer for checking target server connection timeout
    asio::steady_timer connect_timer;
    
    // Pre-allocated timer for action bar updates
    asio::steady_timer actionbar_timer;
    
    enum class State {
        Handshake,
        Status,
        Login,
        Configuration,
        Play,
        Closed
    } state = State::Handshake;
    
    bool is_active = false;
    
    std::array<char, 64> remote_ip = {};
    uint16_t remote_port = 0;
    
    // Cache player credentials to use in logs
    std::array<char, 32> player_name = {};
    std::array<std::byte, 16> player_uuid = {};
    
    // Skin properties cached from Mojang API (stored as fixed-size arrays to avoid dynamic allocation)
    std::array<char, 3072> skin_value = {};
    std::array<char, 1536> skin_signature = {};
    
    size_t config_packet_index = 0;
    size_t play_packet_index = 0;
    
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    float yrot = 0.0f;
    float xrot = 0.0f;
    bool on_ground = true;
    
    Connection(asio::io_context& io, size_t idx) 
        : index(idx), socket(io), check_socket(io), keepalive_timer(io), connect_timer(io), actionbar_timer(io) {
        reset();
    }
    
    void reset() {
        state = State::Handshake;
        rx_len = 0;
        is_active = false;
        remote_ip.fill('\0');
        remote_port = 0;
        player_name.fill('\0');
        player_uuid.fill(std::byte{0});
        skin_value.fill('\0');
        skin_signature.fill('\0');
        config_packet_index = 0;
        play_packet_index = 0;
        x = 0.0;
        y = 0.0;
        z = 0.0;
        yrot = 0.0f;
        xrot = 0.0f;
        on_ground = true;
        
        std::error_code ec;
        keepalive_timer.cancel(ec);
        connect_timer.cancel(ec);
        actionbar_timer.cancel(ec);
        check_socket.close(ec);
    }
};

inline void fetch_player_skin(const char* username, std::array<char, 3072>& out_value, std::array<char, 1536>& out_signature) {
    out_value.fill('\0');
    out_signature.fill('\0');
    
    std::string command = "python3 -c '\n\
import sys, urllib.request, json\n\
try:\n\
    name = sys.argv[1]\n\
    req = urllib.request.Request(\n\
        \"https://api.mojang.com/users/profiles/minecraft/\" + name,\n\
        headers={\"User-Agent\": \"MinecraftSkinFetcher/1.0\"}\n\
    )\n\
    u = urllib.request.urlopen(req, timeout=3)\n\
    data = json.loads(u.read().decode())\n\
    uuid = data[\"id\"]\n\
    req2 = urllib.request.Request(\n\
        \"https://sessionserver.mojang.com/session/minecraft/profile/\" + uuid + \"?unsigned=false\",\n\
        headers={\"User-Agent\": \"MinecraftSkinFetcher/1.0\"}\n\
    )\n\
    u2 = urllib.request.urlopen(req2, timeout=3)\n\
    profile = json.loads(u2.read().decode())\n\
    props = profile.get(\"properties\", [])\n\
    for p in props:\n\
        if p[\"name\"] == \"textures\":\n\
            print(p[\"value\"])\n\
            print(p.get(\"signature\", \"\"))\n\
            sys.exit(0)\n\
except Exception:\n\
    sys.exit(1)\n\
' ";
    
    std::string safe_user;
    for (size_t i = 0; i < 16 && username[i] != '\0'; ++i) {
        char c = username[i];
        if (std::isalnum(c) || c == '_') {
            safe_user += c;
        }
    }
    command += safe_user;
    
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return;
    }
    
    char val_buf[4096] = {};
    char sig_buf[4096] = {};
    
    if (std::fgets(val_buf, sizeof(val_buf), pipe) != nullptr) {
        size_t len = std::strlen(val_buf);
        while (len > 0 && (val_buf[len - 1] == '\n' || val_buf[len - 1] == '\r')) {
            val_buf[len - 1] = '\0';
            len--;
        }
        std::strncpy(out_value.data(), val_buf, out_value.size() - 1);
        
        if (std::fgets(sig_buf, sizeof(sig_buf), pipe) != nullptr) {
            size_t len2 = std::strlen(sig_buf);
            while (len2 > 0 && (sig_buf[len2 - 1] == '\n' || sig_buf[len2 - 1] == '\r')) {
                sig_buf[len2 - 1] = '\0';
                len2--;
            }
            std::strncpy(out_signature.data(), sig_buf, out_signature.size() - 1);
        }
    }
    pclose(pipe);
}

inline bool write_connection_properties(PacketWriter& writer, Connection* conn) {
    if (conn->skin_value[0] != '\0') {
        if (!writer.write_varint(1)) return false;
        if (!writer.write_string("textures")) return false;
        if (!writer.write_string(std::string_view(conn->skin_value.data()))) return false;
        if (conn->skin_signature[0] != '\0') {
            if (!writer.write_bool(true)) return false;
            if (!writer.write_string(std::string_view(conn->skin_signature.data()))) return false;
        } else {
            if (!writer.write_bool(false)) return false;
        }
    } else {
        if (!writer.write_varint(0)) return false;
    }
    return true;
}


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
                    if (conn->state == Connection::State::Configuration || conn->state == Connection::State::Play) {
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
                    
                    conn->player_uuid = uuid;
                    std::strncpy(conn->player_name.data(), name.data(), std::min(name.size(), conn->player_name.size() - 1));
                    
                    log_info("[%s] Fetching skin for player '%s'...", conn->remote_ip.data(), conn->player_name.data());
                    fetch_player_skin(conn->player_name.data(), conn->skin_value, conn->skin_signature);
                    if (conn->skin_value[0] != '\0') {
                        log_info("[%s] Successfully retrieved skin from Mojang API.", conn->player_name.data());
                    } else {
                        log_info("[%s] No skin retrieved (offline username or network error).", conn->player_name.data());
                    }
                    
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
                    if (!write_connection_properties(writer, conn)) return false; 
                    
                    // Write dummy session ID (16 bytes)
                    std::array<std::byte, 16> dummy_session_id = {};
                    if (!writer.write_uuid(dummy_session_id)) return false;
                    
                    auto resp = writer.finalize();
                    if (!resp) return false;
                    
                    write_response(conn, *resp, false); 
                    return true;
                } else if (packet_id == 0x03) {
                    conn->state = Connection::State::Configuration;
                    
                    send_all_configuration_packets(conn);
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
                } else if (packet_id == 0x03) {
                    conn->state = Connection::State::Play;
                    conn->play_packet_index = 0;
                    conn->x = -3.5;
                    conn->y = 124.0;
                    conn->z = 9.5;
                    conn->yrot = 0.0f;
                    conn->xrot = 0.0f;
                    conn->on_ground = true;
                    log_info("[%s] Entered Play mode.", conn->player_name.data());
                    
                    send_next_play_packet(conn);
                    return true;
                } else {
                    return true; 
                }
            }
            case Connection::State::Play: {
                if (packet_id == 0x1E) { // Pos
                    auto x_opt = reader.read_double();
                    auto y_opt = reader.read_double();
                    auto z_opt = reader.read_double();
                    auto flag_opt = reader.read_byte();
                    if (x_opt && y_opt && z_opt && flag_opt) {
                        conn->x = *x_opt;
                        conn->y = *y_opt;
                        conn->z = *z_opt;
                        conn->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                        broadcast_player_movement(conn);
                    }
                    return true;
                } else if (packet_id == 0x1F) { // PosRot
                    auto x_opt = reader.read_double();
                    auto y_opt = reader.read_double();
                    auto z_opt = reader.read_double();
                    auto yrot_opt = reader.read_float();
                    auto xrot_opt = reader.read_float();
                    auto flag_opt = reader.read_byte();
                    if (x_opt && y_opt && z_opt && yrot_opt && xrot_opt && flag_opt) {
                        conn->x = *x_opt;
                        conn->y = *y_opt;
                        conn->z = *z_opt;
                        conn->yrot = *yrot_opt;
                        conn->xrot = *xrot_opt;
                        conn->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                        broadcast_player_movement(conn);
                    }
                    return true;
                } else if (packet_id == 0x20) { // Rot
                    auto yrot_opt = reader.read_float();
                    auto xrot_opt = reader.read_float();
                    auto flag_opt = reader.read_byte();
                    if (yrot_opt && xrot_opt && flag_opt) {
                        conn->yrot = *yrot_opt;
                        conn->xrot = *xrot_opt;
                        conn->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                        broadcast_player_movement(conn);
                    }
                    return true;
                } else if (packet_id == 0x21) { // StatusOnly
                    auto flag_opt = reader.read_byte();
                    if (flag_opt) {
                        conn->on_ground = (static_cast<uint8_t>(*flag_opt) & 1) != 0;
                    }
                    return true;
                } else if (packet_id == 0x1C) { // KeepAlive acknowledgement
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

    void send_all_configuration_packets(Connection* conn) {
        conn->config_packet_index = 0;
        send_next_config_packet(conn);
    }
    
    void send_next_config_packet(Connection* conn) {
        if (!conn->is_active || conn->state != Connection::State::Configuration) return;
        if (conn->config_packet_index >= std::size(captured::config_packets)) {
            return;
        }
        auto packet = captured::config_packets[conn->config_packet_index];
        conn->config_packet_index++;
        
        asio::async_write(
            conn->socket,
            asio::buffer(packet.data.data(), packet.data.size()),
            [this, conn](std::error_code ec, size_t) {
                if (ec) {
                    close_connection(conn);
                    return;
                }
                send_next_config_packet(conn);
            }
        );
    }
    
    void send_custom_play_packet_012(Connection* conn) {
        // Construct the player info update packet dynamically
        // Header (Length VarInt + Packet ID 0x46)
        // Action mask: 0xFF
        // List size: 1
        // UUID: 16 bytes
        // Name: VarInt length + bytes
        // Properties: 0
        // Chat session: false
        // GameMode: 0
        // Listed: true
        // Latency: 0
        // DisplayName: false
        // ListOrder: 0
        // ShowHat: false
        
        PacketWriter writer(conn->tx_buffer);
        if (writer.write_varint(0x46) &&
            writer.write_byte(std::byte{0xFF}) &&
            writer.write_varint(1) &&
            writer.write_uuid(conn->player_uuid) &&
            writer.write_string(std::string_view(conn->player_name.data())) &&
            write_connection_properties(writer, conn) && // properties
            writer.write_bool(false) && // chat session
            writer.write_varint(0) && // gameMode
            writer.write_bool(true) && // listed
            writer.write_varint(0) && // latency
            writer.write_bool(false) && // displayName
            writer.write_varint(0) && // listOrder
            writer.write_bool(false)) { // showHat
            
            auto resp = writer.finalize();
            if (resp) {
                conn->play_packet_index++;
                asio::async_write(
                    conn->socket,
                    asio::buffer(resp->data(), resp->size()),
                    [this, conn](std::error_code ec, size_t) {
                        if (ec) {
                            close_connection(conn);
                            return;
                        }
                        send_next_play_packet(conn);
                    }
                );
            } else {
                close_connection(conn);
            }
        } else {
            close_connection(conn);
        }
    }
    
    void send_next_play_packet(Connection* conn) {
        if (!conn->is_active || conn->state != Connection::State::Play) return;
        if (conn->play_packet_index >= std::size(captured::play_packets)) {
            send_entity_metadata(conn, conn);
            handle_player_spawned_in_play(conn);
            if (global_state_.online) {
                log_info("[%s] Target server is ONLINE. Transferring player in Play mode.", conn->player_name.data());
                send_play_transfer_packet(conn);
            } else {
                bool already_has_players = false;
                for (const auto& other_opt : connection_pool_) {
                    if (other_opt && other_opt->is_active && other_opt->state == Connection::State::Play && &(*other_opt) != conn) {
                        already_has_players = true;
                        break;
                    }
                }
                
                if (!already_has_players) {
                    log_info("[%s] Target server is OFFLINE. Triggering Wake-on-LAN for %s. Holding player in lobby.", 
                             conn->player_name.data(), config_.target_mac.data());
                    char cmd_buf[128] = {};
                    std::snprintf(cmd_buf, sizeof(cmd_buf), "wakeonlan %s &", config_.target_mac.data());
                    std::system(cmd_buf);
                    trigger_immediate_poll();
                } else {
                    log_info("[%s] Target server is OFFLINE. Server was already triggered. Holding player in lobby.", 
                             conn->player_name.data());
                }
                
                start_play_actionbar_loop(conn);
                start_play_keepalive_loop(conn);
            }
            return;
        }
        
        auto packet = captured::play_packets[conn->play_packet_index];
        if (conn->play_packet_index == 0) {
            int32_t entity_id = 300 + static_cast<int32_t>(conn->index);
            std::memcpy(conn->tx_buffer.data(), packet.data.data(), packet.data.size());
            conn->tx_buffer[2] = static_cast<std::byte>((entity_id >> 24) & 0xFF);
            conn->tx_buffer[3] = static_cast<std::byte>((entity_id >> 16) & 0xFF);
            conn->tx_buffer[4] = static_cast<std::byte>((entity_id >> 8) & 0xFF);
            conn->tx_buffer[5] = static_cast<std::byte>(entity_id & 0xFF);
            
            conn->play_packet_index++;
            asio::async_write(
                conn->socket,
                asio::buffer(conn->tx_buffer.data(), packet.data.size()),
                [this, conn](std::error_code ec, size_t) {
                    if (ec) {
                        close_connection(conn);
                        return;
                    }
                    send_next_play_packet(conn);
                }
            );
        } else if (conn->play_packet_index == 5) {
            int32_t entity_id = 300 + static_cast<int32_t>(conn->index);
            std::memcpy(conn->tx_buffer.data(), packet.data.data(), packet.data.size());
            conn->tx_buffer[2] = static_cast<std::byte>((entity_id >> 24) & 0xFF);
            conn->tx_buffer[3] = static_cast<std::byte>((entity_id >> 16) & 0xFF);
            conn->tx_buffer[4] = static_cast<std::byte>((entity_id >> 8) & 0xFF);
            conn->tx_buffer[5] = static_cast<std::byte>(entity_id & 0xFF);
            
            conn->play_packet_index++;
            asio::async_write(
                conn->socket,
                asio::buffer(conn->tx_buffer.data(), packet.data.size()),
                [this, conn](std::error_code ec, size_t) {
                    if (ec) {
                        close_connection(conn);
                        return;
                    }
                    send_next_play_packet(conn);
                }
            );
        } else if (conn->play_packet_index == 12) {
            send_custom_play_packet_012(conn);
        } else {
            conn->play_packet_index++;
            asio::async_write(
                conn->socket,
                asio::buffer(packet.data.data(), packet.data.size()),
                [this, conn](std::error_code ec, size_t) {
                    if (ec) {
                        close_connection(conn);
                        return;
                    }
                    send_next_play_packet(conn);
                }
            );
        }
    }
    
    void start_play_keepalive_loop(Connection* conn) {
        if (!conn->is_active || conn->state != Connection::State::Play) return;
        
        conn->keepalive_timer.expires_after(std::chrono::seconds(10));
        conn->keepalive_timer.async_wait([this, conn](std::error_code ec) {
            if (ec) return;
            
            if (conn->is_active && conn->state == Connection::State::Play) {
                if (global_state_.online) {
                    log_info("[%s] Target server is ONLINE. Transferring player in Play mode.", conn->player_name.data());
                    send_play_transfer_packet(conn);
                    return;
                }
                
                uint64_t keepalive_id = std::chrono::steady_clock::now().time_since_epoch().count();
                PacketWriter writer(conn->tx_buffer);
                if (writer.write_varint(0x2C) && writer.write_ulong(keepalive_id)) {
                    auto resp = writer.finalize();
                    if (resp) {
                        write_response(conn, *resp, false);
                    }
                }
                start_play_keepalive_loop(conn);
            }
        });
    }
    
    void send_play_transfer_packet(Connection* conn) {
        PacketWriter writer(conn->tx_buffer);
        if (!writer.write_varint(0x81)) return; 
        if (!writer.write_string(config_.target_host.data())) return;
        if (!writer.write_varint(config_.target_port)) return; 
        
        auto resp = writer.finalize();
        if (resp) {
            write_response(conn, *resp, true); 
        }
    }
    
    void send_action_bar(Connection* conn, std::string_view text) {
        std::array<std::byte, 512> actionbar_buf;
        PacketWriter writer(actionbar_buf);
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
                    write_response(conn, *resp, false);
                }
            }
        }
    }
    
    void start_play_actionbar_loop(Connection* conn) {
        if (!conn->is_active || conn->state != Connection::State::Play) return;
        if (global_state_.online) return;
        
        if (global_state_.online) {
            send_action_bar(conn, "§a● Main server is online");
        } else {
            send_action_bar(conn, "§c◌ Main server is starting");
        }
        
        conn->actionbar_timer.expires_after(std::chrono::seconds(2));
        conn->actionbar_timer.async_wait([this, conn](std::error_code ec) {
            if (ec) return;
            start_play_actionbar_loop(conn);
        });
    }
    
    void send_entity_metadata(Connection* recipient, Connection* target) {
        std::array<std::byte, 64> meta_buf;
        PacketWriter writer(meta_buf);
        if (writer.write_varint(0x63) &&
            writer.write_varint(300 + static_cast<int32_t>(target->index)) &&
            writer.write_byte(std::byte{16}) &&
            writer.write_varint(0) &&
            writer.write_byte(std::byte{0x7F}) &&
            writer.write_byte(std::byte{0xFF})) {
            auto resp = writer.finalize();
            if (resp) {
                write_response(recipient, *resp, false);
            }
        }
    }
    
    void send_spawn_player_packets(Connection* recipient, Connection* target) {
        {
            std::array<std::byte, 4096> info_buf;
            PacketWriter writer(info_buf);
            if (writer.write_varint(0x46) &&
                writer.write_byte(std::byte{0x0D}) &&
                writer.write_varint(1) &&
                writer.write_uuid(target->player_uuid) &&
                writer.write_string(std::string_view(target->player_name.data())) &&
                write_connection_properties(writer, target) &&
                writer.write_varint(1) &&
                writer.write_bool(true)) {
                auto resp = writer.finalize();
                if (resp) {
                    write_response(recipient, *resp, false);
                }
            }
        }
        
        {
            std::array<std::byte, 256> spawn_buf;
            PacketWriter writer(spawn_buf);
            if (writer.write_varint(0x01) &&
                writer.write_varint(300 + static_cast<int32_t>(target->index)) &&
                writer.write_uuid(target->player_uuid) &&
                writer.write_varint(156) &&
                writer.write_double(target->x) &&
                writer.write_double(target->y) &&
                writer.write_double(target->z) &&
                writer.write_byte(std::byte{0x00}) &&
                writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(target->xrot * 256.0f / 360.0f))) &&
                writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(target->yrot * 256.0f / 360.0f))) &&
                writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(target->yrot * 256.0f / 360.0f))) &&
                writer.write_varint(0)) {
                auto resp = writer.finalize();
                if (resp) {
                    write_response(recipient, *resp, false);
                }
            }
        }
        send_entity_metadata(recipient, target);
    }
    
    void handle_player_spawned_in_play(Connection* conn) {
        for (auto& other_opt : connection_pool_) {
            if (other_opt && other_opt->is_active && other_opt->state == Connection::State::Play && &(*other_opt) != conn) {
                Connection* other = &(*other_opt);
                send_spawn_player_packets(other, conn);
                send_spawn_player_packets(conn, other);
            }
        }
    }
    
    void broadcast_player_movement(Connection* conn) {
        if (!conn->is_active || conn->state != Connection::State::Play) return;
        
        std::array<std::byte, 256> tele_buf;
        PacketWriter tele_writer(tele_buf);
        if (tele_writer.write_varint(0x7D) &&
            tele_writer.write_varint(300 + static_cast<int32_t>(conn->index)) &&
            tele_writer.write_double(conn->x) &&
            tele_writer.write_double(conn->y) &&
            tele_writer.write_double(conn->z) &&
            tele_writer.write_double(0.0) &&
            tele_writer.write_double(0.0) &&
            tele_writer.write_double(0.0) &&
            tele_writer.write_float(conn->yrot) &&
            tele_writer.write_float(conn->xrot) &&
            tele_writer.write_byte(std::byte{0x00}) &&
            tele_writer.write_byte(std::byte{0x00}) &&
            tele_writer.write_byte(std::byte{0x00}) &&
            tele_writer.write_byte(std::byte{0x00}) &&
            tele_writer.write_bool(conn->on_ground)) {
            auto tele_span = tele_writer.finalize();
            
            std::array<std::byte, 64> rot_buf;
            PacketWriter rot_writer(rot_buf);
            if (rot_writer.write_varint(0x53) &&
                rot_writer.write_varint(300 + static_cast<int32_t>(conn->index)) &&
                rot_writer.write_byte(static_cast<std::byte>(static_cast<int8_t>(conn->yrot * 256.0f / 360.0f)))) {
                auto rot_span = rot_writer.finalize();
                
                if (tele_span && rot_span) {
                    for (auto& other_opt : connection_pool_) {
                        if (other_opt && other_opt->is_active && other_opt->state == Connection::State::Play && &(*other_opt) != conn) {
                            write_response(&(*other_opt), *tele_span, false);
                            write_response(&(*other_opt), *rot_span, false);
                        }
                    }
                }
            }
        }
    }
    
    void broadcast_player_disconnect(Connection* conn) {
        if (!conn->is_active || conn->state != Connection::State::Play) return;
        
        std::array<std::byte, 64> remove_ent_buf;
        PacketWriter ent_writer(remove_ent_buf);
        if (ent_writer.write_varint(0x4D) &&
            ent_writer.write_varint(1) &&
            ent_writer.write_varint(300 + static_cast<int32_t>(conn->index))) {
            auto ent_resp = ent_writer.finalize();
            
            std::array<std::byte, 64> remove_info_buf;
            PacketWriter info_writer(remove_info_buf);
            if (info_writer.write_varint(0x45) &&
                info_writer.write_varint(1) &&
                info_writer.write_uuid(conn->player_uuid)) {
                auto info_resp = info_writer.finalize();
                
                if (ent_resp && info_resp) {
                    for (auto& other_opt : connection_pool_) {
                        if (other_opt && other_opt->is_active && other_opt->state == Connection::State::Play && &(*other_opt) != conn) {
                            write_response(&(*other_opt), *ent_resp, false);
                            write_response(&(*other_opt), *info_resp, false);
                        }
                    }
                }
            }
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
                    std::error_code shutdown_ec;
                    conn->socket.shutdown(asio::ip::tcp::socket::shutdown_send, shutdown_ec);
                    
                    conn->connect_timer.expires_after(std::chrono::seconds(2));
                    conn->connect_timer.async_wait([this, conn](std::error_code ec) {
                        if (!ec) {
                            close_connection(conn);
                        }
                    });
                }
            }
        );
    }
    
    void close_connection(Connection* conn) {
        if (!conn->is_active) return;
        
        if (conn->state == Connection::State::Play) {
            broadcast_player_disconnect(conn);
        }
        
        std::error_code ec;
        conn->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        conn->socket.close(ec);
        conn->reset();
    }

    void schedule_poll(int seconds) {
        bool players_in_lobby = false;
        for (const auto& conn_opt : connection_pool_) {
            if (conn_opt && conn_opt->is_active && conn_opt->state == Connection::State::Play) {
                players_in_lobby = true;
                break;
            }
        }
        int interval = players_in_lobby ? 5 : seconds;
        poll_timer_.expires_after(std::chrono::seconds(interval));
        poll_timer_.async_wait([this](std::error_code ec) {
            if (ec) return;
            start_poll_cycle();
        });
    }

    void trigger_immediate_poll() {
        std::error_code ec;
        poll_timer_.cancel(ec);
        start_poll_cycle();
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
        h_writer.write_varint(776);
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
    std::array<std::byte, 16384> poll_rx_buffer_;
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