/* client.h
 * TCP client: connect to server, send/recv messages and files, event queue.
 */

#ifndef CLIENT_H
#define CLIENT_H

#include "protocol.h"

#define EVENT_QUEUE_SIZE 256

typedef enum {
    EVT_CHAT,
    EVT_FILE_PROGRESS,
    EVT_FILE_COMPLETE,
    EVT_FILE_ERROR
} EventType;

typedef struct {
    EventType type;
    char from[MAX_NAME];
    char text[MAX_MSG];
    long timestamp;
    /* file transfer fields */
    int      xfer_id;
    char     filename[256];
    uint32_t done_chunks;
    uint32_t total_chunks;
    XferState xfer_state;
} ChatEvent;

#ifdef __cplusplus
extern "C" {
#endif

int  client_connect(const char *ip, uint16_t port, const char *username);
void client_disconnect(void);
int  client_send_chat(const char *to, const char *text);
int  client_send_file(const char *filepath, const char *to);
int  client_pause_transfer(int xfer_id);
int  client_resume_transfer(int xfer_id);
int  client_poll_event(ChatEvent *out);
int  client_is_connected(void);
const char *client_get_username(void);
int  client_get_sock_fd(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_H */
