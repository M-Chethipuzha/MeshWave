/* server.h
 * TCP server: accept connections, maintain peer list, route messages.
 */

#ifndef SERVER_H
#define SERVER_H

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void server_start(const char *name);
void server_stop(void);
int  server_get_peers(Peer *out, int max);
int  server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */
