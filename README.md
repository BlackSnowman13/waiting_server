# WaitingServer

A lightweight lobby stub server for Minecraft Java Edition version **26.1.2**. 

This application is designed to act as an automated **Wake-on-LAN (WoL) trigger gate**. When a player attempts to log in, the server triggers a WoL packet to wake up a primary offline Minecraft server, holds the player in a stable configuration lobby using Keep-Alive packets, and transparently redirects (transfers) the player to the primary server once it comes online.

---

## Key Features & Optimization

* **Raspberry Pi Zero 2 W Optimized**: Engineered specifically for ultra-low resource, single-board computers. 
* **Zero Runtime Heap Allocation**: Employs static pooling for active connections and pre-allocated stack buffers to guarantee minimal CPU latency and prevent memory fragmentation.
* **Low Idle Footprint**: Operates with a minimal memory footprint and consumes virtually zero CPU cycles while polling the target server in the background.

---

## Prerequisites

Ensure you have the following installed on your machine:

* **CMake** (version 3.22 or higher)
* A modern C++ compiler supporting **C++20** (e.g., `g++` 11+ or `clang++` 13+)
* **wakeonlan** utility (required to broadcast magic packets to target MAC addresses)

---

## Getting Started

### 1. Clone the Repository
```bash
git clone https://github.com/BlackSnowman13/waiting_server.git
cd waiting_server
```

### 2. Build the Server
Configure and build the executable using CMake:
```bash
cmake -B build -S .
cmake --build build
```

### 3. Configuration
The application reads its configuration from a `config.txt` file located in the working directory. If it doesn't exist, the server will automatically create a default configuration on its first run.

Create or edit `config.txt` with your target server details:
```ini
target_host=enter-host-here
target_port=25565
target_mac=AA:BB:CC:DD:EE:FF
```

### 4. Run the Server
Launch the server by passing the listening port (defaults to `25565` if omitted):
```bash
./build/WaitingServer 25575
```
