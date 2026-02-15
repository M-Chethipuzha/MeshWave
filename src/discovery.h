/* discovery.h
 * UDP broadcast peer discovery for LAN servers.
 */

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void discovery_start_announce(const char *server_name, uint16_t data_port);
void discovery_stop_announce(void);

void discovery_start_scan(void);
void discovery_stop_scan(void);

int         discovery_get_servers(ServerInfo *out, int max);
const char *discovery_get_local_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* DISCOVERY_H */
