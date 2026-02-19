# Wire Protocol Specification

> Binary protocol for MeshWave peer-to-peer communication over TCP.

---

## 1. Overview

MeshWave uses a custom binary protocol over TCP for all peer communication. The protocol is designed for simplicity and low overhead on local area networks. All multi-byte integers are transmitted in **network byte order (big-endian)**.

Discovery uses a separate **UDP broadcast** mechanism described in [Section 6](#6-discovery-udp).

---

## 2. Packet Format

Every TCP message begins with a 7-byte packed header followed by a variable-length payload.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│    type (8)    │              seq (32)                             │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                │        payload_len (16)       │                   │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                   payload (payload_len bytes)                     │
│                            ...                                    │
└───────────────────────────────────────────────────────────────────┘
```

### Header Structure (C)

```c
typedef struct {
    uint8_t  type;         // MsgType enum value
    uint32_t seq;          // Sequence number (network byte order)
    uint16_t payload_len;  // Length of payload in bytes
} __attribute__((packed)) PktHeader;
```

**Total header size:** 7 bytes (packed, no padding)

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| `type` | 0 | 1 byte | Message type (see Section 3) |
| `seq` | 1 | 4 bytes | Sequence number, big-endian |
| `payload_len` | 5 | 2 bytes | Payload length, big-endian. Max: 65,535 |

---

## 3. Message Types

```c
typedef enum {
    MSG_HELLO      = 0x01,
    MSG_CHAT       = 0x02,
    MSG_FILE_META  = 0x03,
    MSG_FILE_CHUNK = 0x04,
    MSG_FILE_ACK   = 0x05,
    MSG_FILE_NACK  = 0x06,
    MSG_PAUSE      = 0x07,
    MSG_RESUME     = 0x08,
    MSG_BYE        = 0x09,
} MsgType;
```

| Code | Name | Direction | Purpose |
|------|------|-----------|---------|
| `0x01` | `MSG_HELLO` | Client → Server | Register with username |
| `0x02` | `MSG_CHAT` | Bidirectional | Send a text message |
| `0x03` | `MSG_FILE_META` | Sender → Receiver | Initiate file transfer |
| `0x04` | `MSG_FILE_CHUNK` | Sender → Receiver | File data chunk |
| `0x05` | `MSG_FILE_ACK` | Receiver → Sender | Acknowledge chunk receipt |
| `0x06` | `MSG_FILE_NACK` | Receiver → Sender | Request chunk retransmission |
| `0x07` | `MSG_PAUSE` | Either → Either | Pause active transfer |
| `0x08` | `MSG_RESUME` | Either → Either | Resume paused transfer |
| `0x09` | `MSG_BYE` | Client → Server | Graceful disconnect |

---

## 4. Payload Formats

### 4.1 MSG_HELLO (0x01)

Sent by a client immediately after TCP connection to register a username.

```
┌──────────────────────┐
│ username (NUL-term)  │
└──────────────────────┘
```

- `username`: UTF-8 string, null-terminated. Max 63 bytes + NUL.

**Server behavior:** Stores the name in the peer table and broadcasts a join notification to all other peers.

---

### 4.2 MSG_CHAT (0x02)

A text message from one peer to another, routed through the server.

```
┌──────────────────┬───┬──────────────────┐
│ recipient (var)  │\0 │ message (var)     │
└──────────────────┴───┴──────────────────┘
```

- `recipient`: Null-terminated peer name
- `message`: UTF-8 text, remaining bytes after the NUL separator

**Server behavior:** Extracts the recipient name, looks up the peer by name, and forwards the entire packet to the target's socket. If the recipient is not found, the packet is silently dropped.

---

### 4.3 MSG_FILE_META (0x03)

Initiates a file transfer. Sent before any chunks.

```
┌──────────────────┬───┬──────────────────┬───┬──────────────┬──────────────┐
│ recipient (var)  │\0 │ filename (var)    │\0 │ total_chunks │  file_size   │
│                  │   │                   │   │   (4 bytes)  │  (8 bytes)   │
└──────────────────┴───┴──────────────────┴───┴──────────────┴──────────────┘
```

- `recipient`: Null-terminated peer name
- `filename`: Null-terminated file name (basename only, no path)
- `total_chunks`: `uint32_t`, network byte order — number of chunks
- `file_size`: `uint64_t`, network byte order — total file size in bytes

**Receiver behavior:** Creates the output file in `./downloads/`, pre-allocates disk space, initializes a chunk bitmask, and sends `MSG_FILE_ACK` to confirm readiness.

---

### 4.4 MSG_FILE_CHUNK (0x04)

A single chunk of file data.

```
┌──────────────┬──────────────┬─────────────────────┐
│  xfer_id     │  chunk_seq   │   chunk_data        │
│  (4 bytes)   │  (4 bytes)   │   (up to 64 KB)     │
└──────────────┴──────────────┴─────────────────────┘
```

- `xfer_id`: `uint32_t` — transfer identifier
- `chunk_seq`: `uint32_t` — zero-based chunk index
- `chunk_data`: Raw bytes, up to `CHUNK_SIZE` (65,536 bytes). The last chunk may be smaller.

**Receiver behavior:** Writes data at offset `chunk_seq * CHUNK_SIZE` using `pwrite()`. Sets the corresponding bit in the chunk bitmask. Sends `MSG_FILE_ACK` on success or `MSG_FILE_NACK` on write failure.

---

### 4.5 MSG_FILE_ACK (0x05)

Acknowledges successful receipt of a chunk or metadata.

```
┌──────────────┬──────────────┐
│  xfer_id     │  chunk_seq   │
│  (4 bytes)   │  (4 bytes)   │
└──────────────┴──────────────┘
```

- `xfer_id`: Transfer identifier
- `chunk_seq`: The chunk that was successfully received

**Sender behavior:** Advances to the next chunk. If this was the last chunk, transitions the transfer state to `XFER_DONE`.

---

### 4.6 MSG_FILE_NACK (0x06)

Reports a chunk error, requesting retransmission.

```
┌──────────────┬──────────────┐
│  xfer_id     │  chunk_seq   │
│  (4 bytes)   │  (4 bytes)   │
└──────────────┴──────────────┘
```

**Sender behavior:** Retransmits the specified chunk. After 3 consecutive failures on the same chunk, the transfer transitions to `XFER_ERROR`.

---

### 4.7 MSG_PAUSE (0x07)

Pauses an active file transfer.

```
┌──────────────┐
│  xfer_id     │
│  (4 bytes)   │
└──────────────┘
```

**Sender behavior:** Stops sending chunks, holds position. The transfer state becomes `XFER_PAUSED`.

---

### 4.8 MSG_RESUME (0x08)

Resumes a paused file transfer.

```
┌──────────────┐
│  xfer_id     │
│  (4 bytes)   │
└──────────────┘
```

**Sender behavior:** Resumes sending from the last unacknowledged chunk. State returns to `XFER_ACTIVE`.

---

### 4.9 MSG_BYE (0x09)

Graceful disconnect notification.

```
┌──────────────────────┐
│ username (NUL-term)  │
└──────────────────────┘
```

**Server behavior:** Removes the peer from the table, closes the socket, and broadcasts a leave notification to remaining peers.

---

## 5. Transfer State Machine

```c
typedef enum {
    XFER_IDLE,    // Transfer record exists but not started
    XFER_ACTIVE,  // Chunks are being sent/received
    XFER_PAUSED,  // Temporarily halted by user or peer
    XFER_DONE,    // All chunks acknowledged
    XFER_ERROR,   // Unrecoverable error (max retries exceeded)
} XferState;
```

### State Transitions

```
         transfer_send_file()
IDLE ─────────────────────────► ACTIVE
                                  │
                    ┌─────────────┤
                    │             │
              MSG_PAUSE      last ACK
                    │             │
                    ▼             ▼
                 PAUSED         DONE
                    │
              MSG_RESUME
                    │
                    ▼
                 ACTIVE
                    │
              3 retries failed
                    │
                    ▼
                  ERROR
```

### Retry Parameters

| Parameter | Value |
|-----------|-------|
| ACK timeout | 2 seconds |
| Max retries per chunk | 3 |
| Chunk size | 64 KB (65,536 bytes) |

---

## 6. Discovery (UDP)

Discovery operates independently from the TCP data channel.

### Broadcast Packet

Servers send a JSON-encoded UDP datagram every 2 seconds to the broadcast address `255.255.255.255` on port **5556**.

```json
{
  "name": "ServerName",
  "ip": "192.168.1.42",
  "port": 5557,
  "version": 1
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Human-readable server name |
| `ip` | string | Server's IP address |
| `port` | integer | TCP data port (always 5557) |
| `version` | integer | Protocol version (currently 1) |

### Client Discovery

- Clients listen on UDP port 5556
- Incoming broadcasts are parsed and stored in a `ServerInfo` list
- Entries are deduplicated by IP address
- Entries not refreshed within 10 seconds are expired

---

## 7. Connection Lifecycle

```
Client                          Server
  │                               │
  │──── TCP connect ─────────────►│
  │                               │
  │──── MSG_HELLO("Alice") ──────►│  ← registers peer
  │                               │
  │◄─── MSG_CHAT("Bob","hi") ────│  ← routed from Bob
  │                               │
  │──── MSG_FILE_META(...) ──────►│  ← forwarded to Bob
  │◄─── MSG_FILE_ACK ───────────│  ← from Bob
  │                               │
  │──── MSG_FILE_CHUNK[0..N] ───►│  ← forwarded to Bob
  │◄─── MSG_FILE_ACK[0..N] ─────│  ← from Bob
  │                               │
  │──── MSG_BYE("Alice") ───────►│  ← peer removed
  │                               │
  │◄─── TCP close ──────────────│
```

---

## 8. Constants

```c
#define CHUNK_SIZE     (64 * 1024)   // 65,536 bytes per chunk
#define MAX_PEERS      32            // Maximum concurrent peers
#define MAX_TRANSFERS  16            // Maximum concurrent transfers
#define MAX_NAME       64            // Maximum username length
#define MAX_MSG        4096          // Maximum chat message length
#define DISC_PORT      5556          // UDP discovery port
#define DATA_PORT      5557          // TCP data port
#define HTTP_PORT      5558          // HTTP dashboard port
```

---

## 9. Error Handling

| Scenario | Behavior |
|----------|----------|
| Peer disconnects mid-transfer | Transfer state → ERROR, remaining peers unaffected |
| Chunk write fails | NACK sent, sender retries |
| 3 retries exhausted | Transfer state → ERROR, UI notified via SSE |
| Unknown message type | Packet silently ignored |
| Malformed header | Connection closed, peer removed |
| Server shutdown | All peers receive TCP close, auto-reconnect not implemented |
