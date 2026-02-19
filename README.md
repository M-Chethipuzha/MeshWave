# ⚡ MeshWave

> LAN-native messaging and file sharing — zero configuration, single binary, no cloud.

![C](https://img.shields.io/badge/C-00599C?style=flat&logo=c&logoColor=white)
![C++](https://img.shields.io/badge/C++-00599C?style=flat&logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-064F8C?style=flat&logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux-lightgrey)
![License](https://img.shields.io/badge/License-MIT-green)

---

## Overview

MeshWave is a zero-configuration communication tool for local area networks. It compiles into a single binary that auto-discovers peers on the LAN, serves a browser-based dashboard, and enables real-time chat and file transfer — all without internet, cloud accounts, or external dependencies.

Launch the binary on any machine in the network. Pick **Server** or **Client** mode from the browser UI. Multiple servers can coexist; clients discover them automatically via UDP broadcast and switch freely.

### Key Features

- **Zero-config discovery** — servers announce via UDP broadcast; clients find them instantly
- **Real-time chat** — named peers exchange messages routed through a central server
- **Chunked file transfer** — 64 KB chunks with ACK/NACK, automatic retry (3 attempts), pause/resume
- **Embedded web dashboard** — WhatsApp Web-inspired UI served directly from the binary
- **Single binary** — no runtime dependencies, no config files, no installation

---

## Demo

```
$ ./meshwave
[10:32:01.204] meshwave: starting on port 5558
[10:32:01.205] meshwave: opening browser to http://localhost:5558
```

The browser opens to a mode selection screen. Choose **Server** to host or **Client** to join. The sidebar shows discovered servers (client) or connected peers (server). Chat messages appear as bubbles; file transfers show real-time progress bars.

---

## Quick Start

### Prerequisites

| Tool | Version |
|------|---------|
| C/C++ compiler | GCC 11+ or Clang 14+ |
| CMake | 3.20 or newer |
| Python 3 | For HTML embedding at build time |
| A modern browser | Chrome, Firefox, Safari, Edge |

### Build & Run

```bash
git clone https://github.com/mathewthomas/meshwave.git
cd meshwave
mkdir build && cd build
cmake ..
make -j$(nproc)
./meshwave
```

The binary opens `http://localhost:5558` in your default browser.

### Command-Line Options

| Flag | Description |
|------|-------------|
| *(no flags)* | Interactive — browser opens, user picks mode |
| `--server` | Start directly as server (skip mode selection) |
| `--client <IP>` | Start as client, connect to server at `<IP>` |

### Multi-Machine Setup

1. Run `./meshwave --server` on **Machine A**
2. Run `./meshwave` on **Machine B** (same LAN)
3. Machine B's dashboard auto-discovers Machine A in the sidebar
4. Click the server name → enter a username → start chatting

---

## Architecture

MeshWave follows a modular C architecture with a thin C++ layer for HTTP serving:

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   Browser    │◄───►│  http.cpp    │◄───►│  client.c    │
│  (index.html)│ HTTP│  REST + SSE  │Event│  TCP connect  │
└──────────────┘     └──────────────┘Queue└──────┬───────┘
                                                  │ TCP
                     ┌──────────────┐     ┌──────▼───────┐
                     │ discovery.c  │ UDP │  server.c    │
                     │  broadcast   │◄───►│  accept loop │
                     └──────────────┘     └──────┬───────┘
                                                  │
                     ┌──────────────┐             │
                     │ transfer.c   │◄────────────┘
                     │ chunk engine │  File I/O
                     └──────────────┘
```

| Module | Language | Purpose |
|--------|----------|---------|
| `protocol.h` | C | Wire format, enums, constants — the shared vocabulary |
| `discovery.c` | C | UDP broadcast announce (server) and scan (client) |
| `server.c` | C | TCP accept loop, peer table, message/file routing |
| `client.c` | C | TCP connection, send/receive, event queue for UI |
| `transfer.c` | C | Chunked file I/O with ACK/NACK, pause/resume, retry |
| `http.cpp` | C++ | Embedded HTTP/1.1 server, REST API, SSE streaming |
| `main.cpp` | C++ | Entry point, argument parsing, thread orchestration |
| `util.c` | C | Logging (`util_log`) and time helpers |

For a deeper dive, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Protocol

All communication uses a compact binary protocol over TCP with a 7-byte packed header:

```
┌──────┬──────┬─────────────┐
│ type │ seq  │ payload_len │
│ 1B   │ 4B   │ 2B          │
└──────┴──────┴─────────────┘
```

Nine message types cover the full lifecycle:

| Type | Code | Description |
|------|------|-------------|
| `MSG_HELLO` | `0x01` | Peer handshake with username |
| `MSG_CHAT` | `0x02` | Text message to a named peer |
| `MSG_FILE_META` | `0x03` | File transfer initiation (name, size, chunks) |
| `MSG_FILE_CHUNK` | `0x04` | 64 KB data chunk |
| `MSG_FILE_ACK` | `0x05` | Chunk received successfully |
| `MSG_FILE_NACK` | `0x06` | Chunk error — request retransmit |
| `MSG_PAUSE` | `0x07` | Pause active transfer |
| `MSG_RESUME` | `0x08` | Resume paused transfer |
| `MSG_BYE` | `0x09` | Graceful disconnect |

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full wire specification.

---

## Project Structure

```
meshwave/
├── CMakeLists.txt              # Build configuration
├── README.md                   # This file
├── projectdocument.md          # Original project specification
├── docs/
│   ├── ARCHITECTURE.md         # System design and module details
│   ├── BUILDING.md             # Detailed build instructions
│   └── PROTOCOL.md             # Wire protocol specification
├── src/
│   ├── protocol.h              # Shared types and constants
│   ├── util.c / util.h         # Logging and helpers
│   ├── discovery.c / .h        # UDP peer discovery
│   ├── server.c / .h           # TCP server and routing
│   ├── client.c / .h           # TCP client and event queue
│   ├── transfer.c / .h         # Chunked file transfer engine
│   ├── http.cpp / http.h       # Embedded HTTP server
│   └── main.cpp                # Entry point
├── web/
│   └── index.html              # Frontend dashboard (embedded at build time)
└── scripts/
    └── embed_html.py           # HTML → C string converter
```

**Total: ~3,300 lines across 17 source files.**

---

## API Reference

The embedded HTTP server exposes these endpoints:

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/` | Serve the dashboard UI |
| `GET` | `/api/status` | Server/client mode and connection state |
| `POST` | `/api/mode` | Set mode (`{"mode":"server"}` or `{"mode":"client","ip":"..."}`) |
| `GET` | `/api/servers` | Discovered servers (client mode) |
| `POST` | `/api/connect` | Connect to a server `{"ip":"...","name":"..."}` |
| `GET` | `/api/peers` | Connected peers list |
| `POST` | `/api/chat` | Send message `{"to":"peer","text":"hello"}` |
| `GET` | `/api/events` | SSE stream — real-time chat and file events |
| `POST` | `/api/file/send` | Start file transfer `{"path":"/file","to":"peer"}` |
| `POST` | `/api/file/pause` | Pause transfer `{"id":1}` |
| `POST` | `/api/file/resume` | Resume transfer `{"id":1}` |
| `GET` | `/api/transfers` | Status of all active transfers |

---

## Network Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 5556 | UDP | Discovery broadcast |
| 5557 | TCP | Data (chat messages + file chunks) |
| 5558 | TCP | HTTP dashboard |

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| C for networking core | Direct socket API access, minimal overhead, educational value |
| C++ only for HTTP | String handling and `std::thread` simplify HTTP parsing |
| No third-party libraries | Self-contained binary; demonstrates socket programming fundamentals |
| Embedded HTML | Single binary deployment; no filesystem dependency at runtime |
| SSE over WebSocket | Simpler implementation; sufficient for server→client push |
| 64 KB chunks | Balances throughput with memory use; fits in a single TCP segment |
| Bitmask for chunk tracking | O(1) lookup for received chunks; enables resume from any point |

---

## Limitations & Future Work

- **Single subnet only** — discovery uses broadcast, which doesn't cross routers
- **No encryption** — all traffic is plaintext (LAN-only use case)
- **No persistent history** — messages and transfers exist only during the session
- **Sequential chunk ACK** — throughput could improve with sliding window ACK
- **Browser file API** — drag-and-drop passes filename only; full path requires manual input

---

## Contributing

This is a graduate coursework project. If you'd like to extend it:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/your-feature`)
3. Commit your changes (`git commit -m "feat: add your feature"`)
4. Push to the branch (`git push origin feature/your-feature`)
5. Open a pull request

Please follow the existing code style: snake_case functions with module prefixes, PascalCase types, UPPER_SNAKE constants.

---

## License

This project is released under the [MIT License](LICENSE).

---

## Author

**Mathew Thomas**

Graduate Student — Computer Science

Built as a systems programming project demonstrating socket programming, protocol design, and embedded web serving in C/C++.
