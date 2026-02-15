/* protocol.h
 * Wire format, enums, packet structures, and shared constants.
 * This is the first file a reader should open.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

typedef enum {
    MSG_HELLO      = 0x01,
    MSG_CHAT       = 0x02,
    MSG_FILE_META  = 0x03,
    MSG_FILE_CHUNK = 0x04,
    MSG_FILE_ACK   = 0x05,
    MSG_FILE_NACK  = 0x06,
    MSG_PAUSE      = 0x07,
    MSG_RESUME     = 0x08,
    MSG_BYE        = 0x09
} MsgType;

typedef enum {
    XFER_IDLE,
    XFER_ACTIVE,
    XFER_PAUSED,
    XFER_DONE,
    XFER_ERROR
} XferState;

typedef enum {
    MODE_NONE,
    MODE_SERVER,
    MODE_CLIENT
} RunMode;

typedef struct {
    uint8_t  type;
    uint32_t seq;
    uint16_t payload_len;
} __attribute__((packed)) PktHeader;

typedef struct {
    char     name[64];
    char     ip[46];
    uint16_t port;
} ServerInfo;

typedef struct {
    int      fd;
    char     name[64];
    char     addr[46];
    uint16_t port;
    int      active;
} Peer;

typedef struct {
    int        id;
    XferState  state;
    char       filename[256];
    char       peer[64];
    uint32_t   total_chunks;
    uint32_t   done_chunks;
    uint8_t   *chunk_map;
} Transfer;

#define CHUNK_SIZE  (64 * 1024)
#define MAX_PEERS   32
#define DISC_PORT   5556
#define DATA_PORT   5557
#define HTTP_PORT   5558
#define MAX_NAME    64
#define MAX_MSG     4096
#define DISC_INTERVAL_MS  2000
#define DISC_EXPIRE_MS    10000
#define XFER_TIMEOUT_S    2
#define XFER_MAX_RETRIES  3

#endif /* PROTOCOL_H */
