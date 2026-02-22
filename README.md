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

## Platform Support

| Platform | Status | Toolchain |
|----------|--------|-----------|
| **Linux** | ✅ Supported | GCC 11+ |
| **macOS** | ✅ Supported | Clang 14+ (Xcode) |
| **Windows** | ❌ Not supported | — |

### Why not Windows?

MeshWave is built on POSIX APIs that are fundamental to Unix systems programming:

- **POSIX sockets** — `sys/socket.h`, `arpa/inet.h`, `netinet/in.h`
- **POSIX threading** — `pthread_create`, `pthread_mutex_t`, `PTHREAD_MUTEX_INITIALIZER`
- **POSIX I/O** — `pwrite()`, `fcntl()`, `select()` with file descriptors
- **Network interfaces** — `ifaddrs.h`, `getifaddrs()` for local IP detection
- **Packed structs** — `__attribute__((packed))` (GCC/Clang extension)

Windows does not provide these headers or APIs natively. A port would require replacing the entire networking and threading layer with Winsock2 (`ws2_32`), Win32 threads or C11 threads, and MSVC-compatible struct packing (`#pragma pack`). This is a deliberate design choice — the project serves as a systems programming exercise focused on Unix socket programming fundamentals.

For Windows users, running MeshWave inside **WSL2** (Windows Subsystem for Linux) works seamlessly with no code changes.

---

## Limitations & Future Work

- **Unix/macOS only** — requires POSIX APIs; see [Platform Support](#platform-support) above
- **Single subnet only** — discovery uses broadcast, which doesn't cross routers
- **No encryption** — all traffic is plaintext (LAN-only use case)
- **No persistent history** — messages and transfers exist only during the session
- **Sequential chunk ACK** — throughput could improve with sliding window ACK
- **Browser file API** — drag-and-drop passes filename only; full path requires manual input

---

# Future Additions

This section outlines planned and proposed features for future versions of MeshWave, grouped by category.

---

## LAN Game Lobby

MeshWave already has the two core primitives a game lobby needs: peer discovery and real-time messaging. The natural extension is a lightweight session-brokering layer on top.

**How it would work:**

- Servers advertise open game sessions via a new `MSG_GAME_ANNOUNCE` message type alongside the existing UDP broadcast
- Clients see available game sessions in a dedicated lobby panel in the dashboard
- A join handshake (`MSG_GAME_JOIN` / `MSG_GAME_START`) coordinates readiness before the session begins
- Game state is exchanged as JSON payloads over the existing TCP channel — no separate game server needed

**Games that fit naturally within the current architecture:**

| Game | Notes |
|------|-------|
| Battleship | Turn-based, low message frequency |
| Tic-Tac-Toe | Minimal state, good proof-of-concept |
| Uno / Card games | State machine maps cleanly to message types |
| Trivia / Quiz | Server acts as quiz master; broadcast to all peers |

No changes to the wire protocol header format are required — new message type codes can be added within the existing `type` byte.

---

## Shared Clipboard

A persistent, shared clipboard pool visible to all connected peers.

- Any peer can push text snippets, URLs, or small code blocks to the pool via `POST /api/clipboard`
- All peers receive the new entry via the existing SSE event stream
- Entries are timestamped, tagged with the sender's username, and displayed in a dedicated sidebar panel
- One-click copy to local clipboard from the dashboard

This addresses the common workflow of moving a URL or snippet between machines on the same desk without reaching for a cloud tool.

---

## Persistent Announcement Board

A pinned message board that survives the ephemeral chat session.

- Peers can post announcements that are pinned to the top of the dashboard for all connected users
- Posts are stored in a local flat file (e.g., `meshwave_board.json`) and loaded on startup
- Supports basic formatting: title, body, author, and timestamp
- Complements the existing chat by providing a place for standing information (meeting times, shared credentials, build status)

This directly addresses the *no persistent history* limitation listed in the current README.

---

## Optional Encryption (ChaCha20-Poly1305)

All traffic is currently plaintext, which is acceptable for trusted LANs but limits deployment in shared environments like dorms, open offices, or conference networks.

- Encrypt all TCP payloads with ChaCha20-Poly1305 using a pre-shared key exchanged at connect time via a simple Diffie-Hellman handshake
- Implemented as a single vendored `.c` file with no OpenSSL or external dependency — keeps the single-binary promise intact
- Opt-in via a `--encrypt` flag; unencrypted peers can still connect and are shown as ⚠️ in the dashboard
- Adds a new `MSG_KEY_EXCHANGE` handshake type to the protocol

---

## Multi-Subnet Discovery (mDNS/DNS-SD)

Current UDP broadcast discovery is limited to a single subnet. This is a real constraint on managed networks with multiple VLANs (offices, universities, co-working spaces).

- Replace or supplement UDP broadcast with mDNS (RFC 6762) and DNS-SD (RFC 6763) service records
- Servers register as `_meshwave._tcp.local` so any mDNS-capable resolver on the network can find them
- Falls back to manual IP entry (already supported via `--client <IP>`) when mDNS is unavailable
- No router or infrastructure changes required — mDNS is link-local but works across many managed switch configurations

---

## Sliding Window ACK for File Transfers

The current sequential chunk ACK model (`MSG_FILE_ACK` per chunk before sending the next) is the primary throughput bottleneck for file transfers, especially on links with non-trivial round-trip times.

- Implement a sliding window of configurable size (default: 8 outstanding chunks, ~512 KB in flight)
- Sender tracks a window of unacknowledged chunks using the existing bitmask in `transfer.c`
- On `MSG_FILE_NACK`, only the missing chunk is retransmitted — window slides forward for all others
- Window size negotiated at `MSG_FILE_META` time so both sides agree before transfer starts
- Backward-compatible: peers that do not advertise window support fall back to sequential ACK

Expected throughput improvement on a typical gigabit LAN: 5–10× for large files.

---

## Peer Presence & Status

Make the peer list feel like a real communication tool rather than a static list.

- Peers broadcast a heartbeat (`MSG_PING`) every 10 seconds; the server marks peers as *away* after two missed beats and *offline* after five
- Peers can set a status string (Available, Busy, In a meeting, etc.) via `POST /api/status`
- Status and presence shown as colour-coded indicators in the peer sidebar
- Dashboard updates in real time via the existing SSE stream — no polling required

---

## Terminal / CLI Mode (No Browser)

An ncurses-based or ANSI escape code UI that runs entirely in the terminal, skipping the browser dashboard.

- Enables MeshWave over SSH sessions where opening a browser is impractical
- Split-pane layout: peer list on the left, chat on the right, status bar at the bottom
- File transfer progress displayed as ASCII progress bars
- Activated via a `--tui` flag; the HTTP server is not started in this mode
- Fully in the spirit of the single-binary, no-dependencies philosophy

---

## 🗺 Roadmap Summary

| Feature | Complexity | Impact | Dependencies |
|---------|-----------|--------|-------------|
| Shared Clipboard | Low | High | None |
| Peer Presence & Status | Low | Medium | None |
| Persistent Announcement Board | Low | Medium | Flat file I/O |
| Sliding Window ACK | Medium | High | Protocol version bump |
| LAN Game Lobby | Medium | High | New message types |
| Optional Encryption | High | Medium | Vendored crypto |
| Multi-Subnet Discovery (mDNS) | High | Medium | mDNS library or custom impl |
| Terminal / CLI Mode | High | Medium | ncurses or raw ANSI |

Features are ordered within each tier by estimated implementation effort. All proposed additions preserve the zero-configuration, single-binary, no-cloud design principles of MeshWave.

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
