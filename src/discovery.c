/* discovery.c
 * UDP broadcast announce (server) and scan (client).
 * Server sends a JSON beacon every 2s; clients collect and expire entries.
 */

#include "discovery.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <errno.h>

static pthread_t      announce_thread;
static volatile int   announce_running = 0;

static pthread_t      scan_thread;
static volatile int   scan_running = 0;

static ServerInfo     seen_servers[MAX_PEERS];
static long           seen_timestamps[MAX_PEERS];
static int            seen_count = 0;
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

static char           local_ip[46] = "0.0.0.0";

static void detect_local_ip(void)
{
    struct ifaddrs *addrs, *cur;
    if (getifaddrs(&addrs) != 0) return;

    for (cur = addrs; cur; cur = cur->ifa_next) {
        if (!cur->ifa_addr) continue;
        if (cur->ifa_addr->sa_family != AF_INET) continue;
        if (cur->ifa_flags & IFF_LOOPBACK) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)cur->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, local_ip, sizeof(local_ip));
        break;
    }
    freeifaddrs(addrs);
}

const char *discovery_get_local_ip(void)
{
    return local_ip;
}

/* ── Announce (server side) ──────────────────────────────── */

typedef struct {
    char     name[MAX_NAME];
    uint16_t data_port;
} AnnounceCtx;

static void *announce_loop(void *arg)
{
    AnnounceCtx *ctx = (AnnounceCtx *)arg;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { util_log(LOG_ERROR, "announce socket: %s", strerror(errno)); free(ctx); return NULL; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    #ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    #endif

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(DISC_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    detect_local_ip();

    char pkt[512];
    snprintf(pkt, sizeof(pkt),
             "{\"name\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"version\":1}",
             ctx->name, local_ip, ctx->data_port);

    util_log(LOG_INFO, "discovery: announcing as \"%s\" on %s:%d", ctx->name, local_ip, ctx->data_port);

    while (announce_running) {
        sendto(fd, pkt, strlen(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
        usleep(DISC_INTERVAL_MS * 1000);
    }

    close(fd);
    free(ctx);
    return NULL;
}

void discovery_start_announce(const char *server_name, uint16_t data_port)
{
    if (announce_running) return;
    announce_running = 1;

    AnnounceCtx *ctx = (AnnounceCtx *)malloc(sizeof(AnnounceCtx));
    snprintf(ctx->name, MAX_NAME, "%s", server_name);
    ctx->data_port = data_port;

    pthread_create(&announce_thread, NULL, announce_loop, ctx);
}

void discovery_stop_announce(void)
{
    if (!announce_running) return;
    announce_running = 0;
    pthread_join(announce_thread, NULL);
}

/* ── Scan (client side) ──────────────────────────────────── */

static void upsert_server(const char *name, const char *ip, uint16_t port)
{
    long now = util_time_ms();
    pthread_mutex_lock(&seen_lock);

    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen_servers[i].ip, ip) == 0 && seen_servers[i].port == port) {
            snprintf(seen_servers[i].name, MAX_NAME, "%s", name);
            seen_timestamps[i] = now;
            pthread_mutex_unlock(&seen_lock);
            return;
        }
    }

    if (seen_count < MAX_PEERS) {
        snprintf(seen_servers[seen_count].name, MAX_NAME, "%s", name);
        snprintf(seen_servers[seen_count].ip, 46, "%s", ip);
        seen_servers[seen_count].port = port;
        seen_timestamps[seen_count] = now;
        seen_count++;
        util_log(LOG_INFO, "discovery: found server \"%s\" at %s:%d", name, ip, port);
    }

    pthread_mutex_unlock(&seen_lock);
}

static void expire_servers(void)
{
    long now = util_time_ms();
    pthread_mutex_lock(&seen_lock);

    for (int i = 0; i < seen_count; ) {
        if (now - seen_timestamps[i] > DISC_EXPIRE_MS) {
            util_log(LOG_INFO, "discovery: expired server \"%s\"", seen_servers[i].name);
            seen_servers[i] = seen_servers[seen_count - 1];
            seen_timestamps[i] = seen_timestamps[seen_count - 1];
            seen_count--;
        } else {
            i++;
        }
    }

    pthread_mutex_unlock(&seen_lock);
}

static void *scan_loop(void *arg)
{
    (void)arg;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { util_log(LOG_ERROR, "scan socket: %s", strerror(errno)); return NULL; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    #ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    #endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(DISC_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        util_log(LOG_ERROR, "scan bind: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    util_log(LOG_INFO, "discovery: scanning for servers on port %d", DISC_PORT);

    while (scan_running) {
        char buf[512];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);

        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &slen);
        if (n > 0) {
            buf[n] = '\0';

            char name[MAX_NAME] = {0}, ip[46] = {0};
            int port = 0;

            char *p;
            if ((p = strstr(buf, "\"name\":\""))) {
                p += 8;
                char *e = strchr(p, '"');
                if (e) { int len = (int)(e - p); if (len >= MAX_NAME) len = MAX_NAME - 1; strncpy(name, p, len); }
            }
            if ((p = strstr(buf, "\"ip\":\""))) {
                p += 6;
                char *e = strchr(p, '"');
                if (e) { int len = (int)(e - p); if (len >= 46) len = 45; strncpy(ip, p, len); }
            }
            if ((p = strstr(buf, "\"port\":"))) {
                port = atoi(p + 7);
            }

            if (name[0] && ip[0] && port > 0)
                upsert_server(name, ip, (uint16_t)port);
        }

        expire_servers();
    }

    close(fd);
    return NULL;
}

void discovery_start_scan(void)
{
    if (scan_running) return;
    scan_running = 1;
    detect_local_ip();
    pthread_create(&scan_thread, NULL, scan_loop, NULL);
}

void discovery_stop_scan(void)
{
    if (!scan_running) return;
    scan_running = 0;
    pthread_join(scan_thread, NULL);
}

int discovery_get_servers(ServerInfo *out, int max)
{
    int count;
    pthread_mutex_lock(&seen_lock);
    count = seen_count < max ? seen_count : max;
    memcpy(out, seen_servers, count * sizeof(ServerInfo));
    pthread_mutex_unlock(&seen_lock);
    return count;
}
