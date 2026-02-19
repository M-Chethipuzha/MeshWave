# Architecture

> System design and module-level documentation for MeshWave.

---

## 1. High-Level Design

MeshWave is structured as a modular, event-driven system compiled into a single binary. The architecture separates concerns into six functional modules, each implemented as a `.c`/`.h` pair (or `.cpp` for the HTTP layer). Modules communicate through direct function calls and a shared event queue.

```
                        ┌─────────────────────────────────┐
                        │          Browser (UI)            │
                        │     http://localhost:5558        │
                        └────────────┬────────────────────┘
                                     │ HTTP / SSE
                        ┌────────────▼────────────────────┐
                        │         http.cpp                 │
                        │   REST API  ·  SSE endpoint      │
                        │   Serves embedded index.html     │
                        └──┬──────────────────────────┬───┘
                           │ Event Queue              │ API calls
                ┌──────────▼──────────┐    ┌──────────▼──────────┐
                │     client.c        │    │    transfer.c        │
                │  TCP connection     │    │  Chunked file I/O    │
                │  Send/recv msgs     │    │  ACK/NACK/retry      │
                │  Event push         │    │  Pause/resume        │
                └──────────┬──────────┘    └──────────┬──────────┘
                           │ TCP                       │
                ┌──────────▼──────────────────────────▼──┐
                │              server.c                    │
                │   Accept loop · Peer table · Routing     │
                │   MSG_CHAT → target  ·  MSG_FILE → fwd   │
                └──────────┬───────────────────────────────┘
                           │ UDP
                ┌──────────▼──────────┐
                │    discovery.c       │
                │  Broadcast announce  │
                │  Scan & collect      │
                └──────────────────────┘
```

### Threading Model

MeshWave uses `pthread` with the following thread allocation:

| Thread | Module | Lifetime | Purpose |
|--------|--------|----------|---------|
| Main | `main.cpp` | Process lifetime | Arg parsing, module init, waits for shutdown |
| HTTP | `http.cpp` | Process lifetime | Accepts HTTP connections, serves API |
| Discovery | `discovery.c` | Process lifetime | UDP announce (server) or scan (client) |
| TCP Accept | `server.c` | Server mode | Accepts incoming TCP connections |
| TCP Recv | `client.c` | Client mode | Reads packets from server, pushes events |
| Transfer × N | `transfer.c` | Per file | One thread per outgoing file transfer |

---

## 2. Module Details

### 2.1 protocol.h — Shared Vocabulary

This header defines every type, constant, and structure shared across modules. It is the first file any reader should open.

**Key definitions:**
- `MsgType` enum (9 message types: HELLO through BYE)
- `XferState` enum (IDLE, ACTIVE, PAUSED, DONE, ERROR)
- `RunMode` enum (MODE_NONE, MODE_SERVER, MODE_CLIENT)
- `PktHeader` — 7-byte packed struct: `{type(1), seq(4), payload_len(2)}`
- `Peer` — per-connection info: fd, name, address
- `Transfer` — per-file state: id, filename, peer, chunks, bitmask

**Constants:**
| Constant | Value | Purpose |
|----------|-------|---------|
| `CHUNK_SIZE` | 65,536 (64 KB) | File chunk size |
| `MAX_PEERS` | 32 | Maximum concurrent peers |
| `MAX_TRANSFERS` | 16 | Maximum concurrent file transfers |
| `DISC_PORT` | 5556 | UDP discovery port |
| `DATA_PORT` | 5557 | TCP data port |
| `HTTP_PORT` | 5558 | Dashboard HTTP port |

### 2.2 discovery.c — Peer Discovery

**Purpose:** Zero-configuration peer discovery using UDP broadcast.

**Server behavior:**
- `discovery_announce()` spawns a background thread
- Every 2 seconds, broadcasts a JSON payload to `255.255.255.255:5556`
- Payload: `{"name":"<server>","ip":"<addr>","port":5557,"version":1}`

**Client behavior:**
- `discovery_scan()` listens on UDP port 5556 for 500ms
- Collects `ServerInfo` structs (name, IP, port)
- Deduplicates by IP address
- Entries expire after 10 seconds without a heartbeat

**Design choice:** UDP broadcast was chosen over multicast for simplicity. The tradeoff is that discovery is limited to the local broadcast domain (single subnet).

### 2.3 server.c — Connection Hub

**Purpose:** Central routing hub for all TCP communication.

**Architecture:**
- Single `select()`-based event loop (no `epoll` — portability over performance)
- Maintains a `Peer` table with fd, name, and address for each connection
- Listens on `DATA_PORT` (5557) for incoming TCP connections

**Message routing:**
| Message Type | Routing Behavior |
|--------------|------------------|
| `MSG_HELLO` | Registers peer name, broadcasts join notification |
| `MSG_CHAT` | Extracts recipient name from payload, forwards to target fd |
| `MSG_FILE_META` | Extracts recipient, forwards metadata to target |
| `MSG_FILE_CHUNK/ACK/NACK` | Broadcasts to all peers except sender |
| `MSG_PAUSE/RESUME` | Broadcasts to all peers except sender |
| `MSG_BYE` | Removes peer from table, notifies remaining peers |

**Error handling:** If a peer's socket errors, the server removes it from the table and continues. One bad connection never crashes the server.

### 2.4 client.c — User Agent

**Purpose:** Manages the TCP connection to a server and provides the event bridge to the HTTP layer.

**Key components:**

1. **Connection management:** `client_connect()` establishes TCP to server, sends `MSG_HELLO` with username.

2. **Receive loop:** Background thread reads packets from the server socket. Each packet is decoded and pushed to a thread-safe event queue as a `ChatEvent`.

3. **Event queue:** A circular buffer protected by a mutex and condition variable. The HTTP SSE endpoint drains this queue to push events to the browser.

4. **Event types:**
   | EventType | Trigger |
   |-----------|---------|
   | `EVT_CHAT` | Incoming chat message |
   | `EVT_FILE_PROGRESS` | File chunk received/sent |
   | `EVT_FILE_COMPLETE` | Transfer finished |
   | `EVT_FILE_ERROR` | Transfer failed after retries |

5. **File operations:** `client_send_file()`, `client_pause_transfer()`, `client_resume_transfer()` delegate to `transfer.c` and send corresponding protocol messages.

### 2.5 transfer.c — File Transfer Engine

**Purpose:** Chunked file I/O with reliability guarantees.

**Send path:**
1. `transfer_send_file()` creates a `Transfer` record, spawns a sender thread
2. Sender opens the file, calculates total chunks (`file_size / CHUNK_SIZE`)
3. Sends `MSG_FILE_META` with filename, total chunks, and file size
4. For each chunk: read 64 KB → wrap in `MSG_FILE_CHUNK` → send → wait for ACK
5. On timeout (2s) or NACK: retry up to 3 times
6. After 3 failures: set state to `XFER_ERROR`, notify via callback

**Receive path:**
1. `transfer_recv_meta()` creates the output file, pre-allocates space, allocates bitmask
2. `transfer_recv_chunk()` writes data at the correct offset using `pwrite()`
3. Bitmask tracks which chunks have been received (enables resume from any point)
4. When all chunks received: set state to `XFER_DONE`

**State machine:**
```
IDLE ──► ACTIVE ──► DONE
           │  ▲
           ▼  │
         PAUSED
           │
           ▼
         ERROR (after 3 retries)
```

**Memory management:** `malloc` is used only in this module for chunk buffers and bitmasks, with paired `free` on transfer completion.

### 2.6 http.cpp — Web Bridge

**Purpose:** Bridges the TCP networking layer to the browser via HTTP.

**Implementation:** A minimal HTTP/1.1 server that:
- Parses request line and headers manually (no library)
- Routes requests by method + path matching
- Serves `index.html` from a compile-time embedded C string (`web_bundle.h`)
- Returns JSON responses with appropriate `Content-Type` headers

**SSE (Server-Sent Events):**
- `GET /api/events` opens a long-lived HTTP connection
- The server drains the event queue every 100ms
- Events are formatted as `event: <type>\ndata: <json>\n\n`
- Event types: `chat`, `file_progress`, `file_complete`, `file_error`

**Why SSE over WebSocket:** SSE is unidirectional (server→client), which matches our use case. It requires no upgrade handshake, no frame masking, and works with standard HTTP. All client→server communication uses regular POST requests.

### 2.7 main.cpp — Entry Point

**Purpose:** Orchestrates module initialization and thread startup.

**Startup sequence:**
1. Parse command-line arguments (`--server`, `--client <IP>`)
2. Initialize transfer engine (`transfer_init()`)
3. Create downloads directory
4. Start HTTP server on port 5558
5. Open system browser to `http://localhost:5558`
6. Wait for mode selection (if not specified via CLI)
7. Start discovery + server/client threads based on mode
8. Block until shutdown signal

---

## 3. Data Flow Examples

### Chat Message Flow

```
User types message in browser
    │
    ▼
POST /api/chat {"to":"Bob","text":"Hello"}
    │
    ▼
http.cpp → client_send_chat("Bob", "Hello")
    │
    ▼
client.c: build PktHeader{MSG_CHAT} + "Bob\0Hello" → send() to server
    │
    ▼
server.c: parse recipient "Bob" → find Bob's fd → send() packet to Bob
    │
    ▼
Bob's client.c recv_loop: decode MSG_CHAT → push ChatEvent to queue
    │
    ▼
Bob's http.cpp SSE: drain queue → "event: chat\ndata: {...}\n\n"
    │
    ▼
Bob's browser: SSE listener → render chat bubble
```

### File Transfer Flow

```
User selects file and recipient
    │
    ▼
POST /api/file/send {"path":"/path/to/file","to":"Bob"}
    │
    ▼
http.cpp → client_send_file("/path/to/file", "Bob")
    │
    ▼
transfer.c: spawn send_thread → MSG_FILE_META → wait ACK
    │
    ▼
Loop: MSG_FILE_CHUNK[i] → wait ACK → next chunk
    │                         │
    │ (on NACK/timeout)       │ (on ACK)
    ▼                         ▼
  Retry (max 3)           Progress callback → SSE → browser progress bar
    │
    ▼ (all chunks ACKed)
  XFER_DONE → SSE file_complete event
```

---

## 4. Build System

CMake handles the two-stage build:

1. **HTML embedding:** A Python script (`scripts/embed_html.py`) converts `web/index.html` into a C header (`web_bundle.h`) containing the HTML as a `const char[]`. This runs as a custom command before compilation.

2. **Compilation:** All `.c` and `.cpp` files are compiled with `-Wall -Wextra -O2` and linked with `pthread`.

The result is a single self-contained binary with no runtime file dependencies.

---

## 5. Security Considerations

MeshWave is designed for trusted LAN environments:

- **No encryption:** All traffic is plaintext TCP/UDP
- **No authentication:** Any peer can connect with any username
- **No input sanitization on files:** File paths are used as provided
- **Trust model:** Assumes all machines on the LAN are trusted

These are acceptable tradeoffs for a LAN-only tool in a controlled environment. For production use, TLS and authentication would be required.
