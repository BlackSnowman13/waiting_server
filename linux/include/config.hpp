#pragma once
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace waiting_server {

struct Config {
    std::string target_host = "127.0.0.1";
    uint16_t    target_port = 25565;
    std::string target_mac  = "AA:BB:CC:DD:EE:FF";
    std::string listen_address = "0.0.0.0";
    uint16_t    listen_port = 25565;
    uint32_t    poll_interval_sec = 3;
};

inline bool load_config(const std::string& filepath, Config& cfg) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        size_t eq = line.find('=', first);
        if (eq == std::string::npos) continue;

        std::string key = line.substr(first, eq - first);
        std::string val = line.substr(eq + 1);

        auto trim = [](std::string& s) {
            size_t p1 = s.find_first_not_of(" \t\r\n");
            size_t p2 = s.find_last_not_of(" \t\r\n");
            if (p1 == std::string::npos) s.clear();
            else s = s.substr(p1, p2 - p1 + 1);
        };
        trim(key);
        trim(val);

        if (key == "target_host") {
            cfg.target_host = val;
        } else if (key == "target_port") {
            try { cfg.target_port = static_cast<uint16_t>(std::stoi(val)); } catch (...) {}
        } else if (key == "target_mac") {
            cfg.target_mac = val;
        } else if (key == "listen_address") {
            cfg.listen_address = val;
        } else if (key == "listen_port") {
            try { cfg.listen_port = static_cast<uint16_t>(std::stoi(val)); } catch (...) {}
        } else if (key == "poll_interval_seconds") {
            try { cfg.poll_interval_sec = static_cast<uint32_t>(std::stoi(val)); } catch (...) {}
        }
    }
    return true;
}

} // namespace waiting_server
