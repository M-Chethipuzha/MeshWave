# ⚡ MeshWave
### LAN-native Messaging & File Sharing

> C / C++ Backend · HTML/JS Frontend · CMake Build · Single Binary

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Naming & Concept](#2-naming--concept)
3. [Goals & Constraints](#3-goals--constraints)
4. [Directory Layout](#4-directory-layout)
5. [Protocol Design](#5-protocol-design)
6. [Build Phases](#6-build-phases)
7. [Module Specifications](#7-module-specifications)
8. [Frontend](#8-frontend-indexhtml)
9. [CMake Build](#9-cmake-build)
10. [Coding Standards](#10-coding-standards)
11. [File Transfer Detail](#11-phase-2--file-transfer-detail)
12. [Discovery Protocol](#12-discovery-protocol)
13. [Suggested Milestones](#13-suggested-milestones)
14. [Implementation Checklist](#14-implementation-checklist)
15. [Reference Reading](#15-reference-reading)

---

## 1. Project Overview

MeshWave is a zero-configuration, LAN-native communication tool. It runs as a single binary that auto-discovers peers on the local network and exposes a modern chat+file UI served directly from the process. No internet, no cloud, no accounts — just launch and talk.

The binary starts a lightweight embedded HTTP server, opens the browser to its dashboard, and asks the user to pick Server or Client mode. Multiple servers can coexist; clients see all of them in a sidebar list and can switch freely.

| Property | Value |
|----------|-------|
| Language | C (networking core) + C++ (HTTP server, logic) |
| Build system | CMake 3.20+ |
| Output | Single binary: `meshwave` |
| UI | Embedded HTML/CSS/JS (no framework) |
| Transport | TCP (messages) + TCP chunked (files) |
| Discovery | UDP broadcast on LAN |
| Phase 1 | Chat messaging between named peers |
| Phase 2 | Chunked file transfer with pause/resume + error retry |

---

## 2. Naming & Concept

"MeshWave" reflects the peer mesh formed across a LAN and the idea of data flowing like a radio wave through local air — no relay, no cloud. The UI takes inspiration from WhatsApp Web: a left sidebar with two tabs (Messages, Files), a contact/server list, and a main panel.

---

## 3. Goals & Constraints

### 3.1 Hard Constraints

- Readable, graduate-student quality code — no over-engineering
- Minimal source files; every file does one obvious thing
- Comments only at the top of each file describing its purpose
- No inline comment clutter inside functions
- Enums and typedefs used for all states and message types
- No third-party libraries beyond the C/C++ standard library
- CMake build — one `cmake && make`, single output binary

### 3.2 Quality Goals

- Clean on valgrind — no leaks in normal operation
- Graceful disconnect handling on both sides
- File transfers survive packet loss via chunk-level retry

---

## 4. Directory Layout

Minimal, flat where possible:

```
meshwave/
├── CMakeLists.txt
├── src/
│   ├── main.cpp          — entry point, mode selection
│   ├── server.c / .h     — server: accept, broadcast, route msgs
│   ├── client.c / .h     — client: connect, send/recv messages
│   ├── discovery.c / .h  — UDP broadcast peer discovery
│   ├── transfer.c / .h   — chunked file send/recv, pause, retry
│   ├── http.cpp / .h     — embedded HTTP server for dashboard
│   ├── protocol.h        — all enums, types, packet structs
│   └── util.c / .h       — logging, string helpers
└── web/
    └── index.html        — single-file UI (embedded as C string)
```

The `web/` directory is processed at build time: CMake runs a small script that converts `index.html` into a C header (`web_bundle.h`) containing the HTML as a string literal. The binary serves it directly — no filesystem dependency at runtime.

---

## 5. Protocol Design

### 5.1 protocol.h — The Heart of the Project

All wire types, states, and constants live in one header. This is the first file a reader should open.

```c
/* protocol.h — wire format, enums, and shared types */

typedef enum {
    MSG_HELLO      = 0x01,  // peer handshake
    MSG_CHAT       = 0x02,  // chat message
    MSG_FILE_META  = 0x03,  // file transfer init
    MSG_FILE_CHUNK = 0x04,  // file data chunk
    MSG_FILE_ACK   = 0x05,  // chunk acknowledged
    MSG_FILE_NACK  = 0x06,  // chunk error, request retry
    MSG_PAUSE      = 0x07,  // pause transfer
    MSG_RESUME     = 0x08,  // resume transfer
    MSG_BYE        = 0x09,  // graceful disconnect
} MsgType;

typedef enum {
    XFER_IDLE, XFER_ACTIVE, XFER_PAUSED, XFER_DONE, XFER_ERROR
} XferState;

typedef struct {
    uint8_t  type;         // MsgType
    uint32_t seq;          // sequence number (big-endian)
    uint16_t payload_len;  // bytes that follow
} __attribute__((packed)) PktHeader;

#define CHUNK_SIZE  (64 * 1024)   // 64 KB per chunk
#define MAX_PEERS   32
#define DISC_PORT   5556          // UDP discovery
#define DATA_PORT   5557          // TCP data
#define HTTP_PORT   5558          // embedded dashboard
```

### 5.2 Message Flow

| Scenario | Packet sequence |
|----------|----------------|
| Peer joins | Client → `MSG_HELLO` → Server → broadcasts to all peers |
| Chat message | Sender → `MSG_CHAT(to, text)` → Server routes → target client |
| File start | Sender → `MSG_FILE_META(name, size, chunks)` → `MSG_FILE_CHUNK × N` |
| Chunk error | Receiver → `MSG_FILE_NACK(seq)` → Sender retransmits chunk |
| Pause/resume | Either side → `MSG_PAUSE` / `MSG_RESUME` — sender buffers or resumes |
| Disconnect | Client → `MSG_BYE` → Server removes peer, notifies others |

---

## 6. Build Phases

### Phase 1 — Core Chat (Milestone 1)

- UDP discovery: server broadcasts presence every 2s; clients collect server list
- TCP server accepts connections, maintains peer list, routes `MSG_CHAT`
- Embedded HTTP server serves `index.html`; WebSocket or SSE bridge to TCP layer
- UI: sidebar with Messages tab, server list, chat panel — no file tab yet
- Mode selection prompt on first open: "Run as Server" / "Run as Client"

### Phase 2 — File Transfer (Milestone 2)

- `MSG_FILE_META` + `MSG_FILE_CHUNK` protocol implemented in `transfer.c`
- Sender iterates chunks, waits for `MSG_FILE_ACK` before next chunk
- On `MSG_FILE_NACK`, sender retransmits that specific chunk only
- `XFER_PAUSED` state: sender stops sending; `MSG_RESUME` restarts from last ack
- UI: Files tab added, per-file progress bar, Pause/Resume button, error indicator
- Recipient can specify save location via dashboard

---

## 7. Module Specifications

### 7.1 discovery.c

Handles UDP broadcast. Server calls `discovery_announce()` in a background thread every 2 seconds. Client calls `discovery_scan()` which listens for 500ms and returns a list of `ServerInfo` structs (ip, port, name). This list feeds the UI sidebar.

### 7.2 server.c

Accepts TCP connections on `DATA_PORT`. Each peer gets a `Peer` struct with fd, name, addr. A single `select()` loop handles all fds. Incoming `MSG_CHAT` packets are routed by destination name. `MSG_FILE_CHUNK` is forwarded or written to disk depending on whether the server is the final recipient.

### 7.3 client.c

Connects to a chosen server. Sends `MSG_HELLO` with chosen username. Exposes `send_chat(to, text)` and `send_file(path, to)` to the HTTP layer. Incoming packets are pushed to an event queue that the HTTP SSE endpoint drains.

### 7.4 transfer.c

Pure chunked I/O. `send_file_chunked()` opens a file, reads `CHUNK_SIZE` bytes, wraps in `MSG_FILE_CHUNK`, sends, waits for ACK with a 2-second timeout. On timeout or NACK, retransmits. `XferState` tracks current state. A pointer to the state struct is shared with the HTTP layer so the UI can poll progress.

### 7.5 http.cpp

A minimal single-threaded HTTP/1.1 server. Serves `GET /` → `index.html` (from `web_bundle.h`). Provides a REST-ish API:

| Endpoint | Purpose |
|----------|---------|
| `GET /api/peers` | JSON list of connected peers |
| `GET /api/servers` | JSON list of discovered servers (from discovery) |
| `POST /api/chat` | Send a chat message `{to, text}` |
| `GET /api/events` | SSE stream — pushes incoming chat + file events |
| `POST /api/file/send` | Initiate file transfer `{path, to}` |
| `POST /api/file/pause` | Pause active transfer `{id}` |
| `POST /api/file/resume` | Resume paused transfer `{id}` |
| `GET /api/transfers` | JSON status of all transfers |

### 7.6 main.cpp

Parses args, starts discovery, starts HTTP server, opens system browser to `http://localhost:5558`. If `--server` flag given, skips mode-selection and goes straight to server mode. Otherwise, the dashboard UI drives the choice.

---

## 8. Frontend (index.html)

Single HTML file, no build step, no framework. Vanilla JS + CSS. Layout mirrors WhatsApp Web:

| Region | Content |
|--------|---------|
| Left sidebar top | Two tab buttons: 💬 Messages \| 📁 Files |
| Left sidebar mid | Server list (from `/api/servers`) — click to connect |
| Left sidebar bottom | Peer list for current server |
| Main panel (Messages) | Chat bubbles, timestamp, sender name. Input bar at bottom. |
| Main panel (Files) | Drag-and-drop zone, recipient picker, transfer queue with progress bars |
| Mode modal | On first load if no server/client chosen: two big buttons |

SSE connection to `/api/events` keeps UI live. No polling. File progress updates arrive as SSE events with JSON payload `{id, state, percent}`.

---

## 9. CMake Build

```cmake
cmake_minimum_required(VERSION 3.20)
project(meshwave C CXX)

# Embed index.html as a C string header
add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/web_bundle.h
  COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/embed_html.py
          ${CMAKE_SOURCE_DIR}/web/index.html
          ${CMAKE_BINARY_DIR}/web_bundle.h
  DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html
)

add_executable(meshwave
  src/main.cpp src/server.c src/client.c
  src/discovery.c src/transfer.c
  src/http.cpp src/util.c
  ${CMAKE_BINARY_DIR}/web_bundle.h  # triggers rebuild on HTML change
)

target_include_directories(meshwave PRIVATE src ${CMAKE_BINARY_DIR})
target_compile_options(meshwave PRIVATE -Wall -Wextra -O2)
target_link_libraries(meshwave PRIVATE pthread)
```

**Build commands:**

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
./meshwave              # opens browser, asks server or client
./meshwave --server     # start directly as server
./meshwave --client IP  # start directly as client of IP
```

---

## 10. Coding Standards

### 10.1 File Header Convention

Every source file starts with a comment block (3–5 lines max):

```c
/* transfer.c
 * Chunked file send and receive.
 * Handles pause, resume, and per-chunk retry on NACK.
 */
```

### 10.2 Naming

| Thing | Convention |
|-------|------------|
| Types / enums | PascalCase: `MsgType`, `XferState`, `PktHeader` |
| Functions | snake_case with module prefix: `transfer_send()`, `server_route()` |
| Constants / macros | UPPER_SNAKE: `CHUNK_SIZE`, `MAX_PEERS` |
| Local variables | Short, clear: `fd`, `buf`, `len`, `peer`, `chunk_id` |
| Files | `module_name.c / .h` pair, one module per file |

### 10.3 Error Handling

All system calls checked. Errors logged via `util_log(level, msg)` and handled locally — no global `errno` reads outside the calling function. Network errors on a single peer do not crash the server; the peer is removed and others continue.

### 10.4 No Over-engineering

- No class hierarchies — structs + functions
- No templates — plain C where possible, C++ only for `http.cpp`
- No dynamic memory for fixed-size structures — use static arrays with `MAX_PEERS`
- `malloc` only in `transfer.c` for chunk buffers; paired `free` on completion

---

## 11. Phase 2 — File Transfer Detail

### 11.1 Chunk State Machine

The sender maintains an `XferState` enum per active transfer. Transitions:

```
IDLE   → ACTIVE  : transfer_send() called
ACTIVE → PAUSED  : MSG_PAUSE received or user clicks Pause in UI
PAUSED → ACTIVE  : MSG_RESUME received or user clicks Resume
ACTIVE → DONE    : last chunk ACKed
ACTIVE → ERROR   : max retries (3) exceeded on any chunk
```

### 11.2 Retry Logic

For each chunk: send → wait up to 2s for `MSG_FILE_ACK`. On timeout or `MSG_FILE_NACK`, retry. After 3 failures on the same chunk, set `state = XFER_ERROR` and notify UI via SSE. The transfer can be restarted from the failed chunk — the receiver keeps a bitmask of received chunks so the sender can skip already-delivered ones.

### 11.3 Transfer Record

```c
typedef struct {
    int        id;
    XferState  state;
    char       filename[256];
    char       peer[64];
    uint32_t   total_chunks;
    uint32_t   done_chunks;
    uint8_t   *chunk_map;   // bitmask, malloc'd
} Transfer;
```

---

## 12. Discovery Protocol

Server broadcasts a UDP packet every 2 seconds on the LAN broadcast address (`255.255.255.255`) on `DISC_PORT`. Payload is a small JSON object:

```json
{"name":"MyServer","ip":"192.168.1.42","port":5557,"version":1}
```

Clients run a background thread that listens on `DISC_PORT` and maintains a list of seen servers (deduped by IP). The list expires entries not heard from in 10 seconds. The HTTP endpoint `/api/servers` returns this list. If there is exactly one server, the UI can auto-connect; with multiple, it shows the picker.

---

## 13. Suggested Milestones

| Milestone | Deliverable |
|-----------|-------------|
| M1 — Skeleton | CMake builds, binary runs, browser opens, mode modal shown |
| M2 — Discovery | Servers appear in sidebar list via UDP broadcast |
| M3 — Connect | Client connects to chosen server; peers appear in peer list |
| M4 — Chat | Messages flow end-to-end, appear in UI bubbles via SSE |
| M5 — File Meta | File tab shown; sender picks file + recipient, META sent |
| M6 — Chunked TX | File arrives complete; progress bar updates in UI |
| M7 — Pause/Resume | Pause and resume work; chunk bitmask tracked |
| M8 — Retry | NACK triggers retransmit; 3-failure error shown in UI |
| M9 — Polish | Multiple simultaneous transfers; clean disconnect; valgrind clean |

---

## 14. Implementation Checklist

### Phase 1

- [ ] `protocol.h` — enums, PktHeader, constants
- [ ] `util.c` — `util_log()`, time helpers
- [ ] `discovery.c` — announce thread + scan function
- [ ] `server.c` — accept loop, peer table, `MSG_CHAT` routing
- [ ] `client.c` — connect, `send_chat()`, event queue
- [ ] `http.cpp` — serve `index.html`, `/api/peers`, `/api/servers`, `/api/chat`, `/api/events` SSE
- [ ] `main.cpp` — arg parse, thread startup, browser open
- [ ] `web/index.html` — sidebar, chat panel, SSE client, mode modal
- [ ] `scripts/embed_html.py` — HTML to C string header
- [ ] `CMakeLists.txt` — embed + compile

### Phase 2

- [ ] `transfer.c` — Transfer struct, send loop, ACK wait, retry, pause/resume
- [ ] `server.c` — route `MSG_FILE_*` to target peer
- [ ] `http.cpp` — `/api/file/send`, `/api/file/pause`, `/api/file/resume`, `/api/transfers`
- [ ] `web/index.html` — Files tab, progress bars, Pause/Resume buttons, error badge

---

## 15. Reference Reading

For a new contributor, the recommended read order is:

1. `protocol.h` — understand the wire format and all enums first
2. `main.cpp` — see how threads and modules are wired together
3. `discovery.c` — simple, self-contained, good warm-up
4. `server.c` then `client.c` — core networking logic
5. `transfer.c` — most complex; read after the others
6. `http.cpp` — glue between TCP layer and browser
7. `web/index.html` — frontend last; depends on API understanding

---

*MeshWave is intentionally small. The entire project should fit comfortably under 3000 lines of code across all files, making it feasible to read end-to-end in an afternoon.*