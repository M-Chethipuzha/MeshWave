/* client.h
 * TCP client: connect to server, send/recv messages, event queue.
 */

#ifndef CLIENT_H
#define CLIENT_H

#include "protocol.h"

#define EVENT_QUEUE_SIZE 256

typedef struct {
    char from[MAX_NAME];
    char text[MAX_MSG];
    long timestamp;
} ChatEvent;

#ifdef __cplusplus
extern "C" {
#endif

int  client_connect(const char *ip, uint16_t port, const char *username);
void client_disconnect(void);
int  client_send_chat(const char *to, const char *text);
int  client_poll_event(ChatEvent *out);
int  client_is_connected(void);
const char *client_get_username(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_H */
