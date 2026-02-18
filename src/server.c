/* server.c
 * TCP server: accept loop, peer table, route chat and file messages.
 * Uses select() to multiplex all peer connections.
 */

#include "server.h"
#include "discovery.h"
#include "transfer.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>

static Peer           peers[MAX_PEERS];
static int            peer_count = 0;
static pthread_mutex_t peer_lock = PTHREAD_MUTEX_INITIALIZER;

static int            listen_fd = -1;
static pthread_t      server_thread;
static volatile int   running = 0;
static char           server_name[MAX_NAME];

static void peer_add(int fd, const char *addr, uint16_t port)
{
    pthread_mutex_lock(&peer_lock);
    if (peer_count < MAX_PEERS) {
        memset(&peers[peer_count], 0, sizeof(Peer));
        peers[peer_count].fd = fd;
        snprintf(peers[peer_count].addr, 46, "%s", addr);
        peers[peer_count].port = port;
        peers[peer_count].active = 1;
        snprintf(peers[peer_count].name, MAX_NAME, "peer_%d", fd);
        peer_count++;
    }
    pthread_mutex_unlock(&peer_lock);
}

static void peer_remove(int fd)
{
    pthread_mutex_lock(&peer_lock);
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].fd == fd) {
            util_log(LOG_INFO, "server: peer \"%s\" disconnected", peers[i].name);
            close(peers[i].fd);
            peers[i] = peers[peer_count - 1];
            peer_count--;
            break;
        }
    }
    pthread_mutex_unlock(&peer_lock);
}

static Peer *peer_find_by_name(const char *name)
{
    for (int i = 0; i < peer_count; i++)
        if (strcmp(peers[i].name, name) == 0)
            return &peers[i];
    return NULL;
}

static int send_all(int fd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p   += n;
        len -= (int)n;
    }
    return 0;
}

static void broadcast_to_all(const void *buf, int len, int exclude_fd)
{
    pthread_mutex_lock(&peer_lock);
    for (int i = 0; i < peer_count; i++) {
        if (peers[i].fd != exclude_fd)
            send_all(peers[i].fd, buf, len);
    }
    pthread_mutex_unlock(&peer_lock);
}

static void handle_packet(int fd, PktHeader *hdr, const char *payload)
{
    switch (hdr->type) {

    case MSG_HELLO: {
        pthread_mutex_lock(&peer_lock);
        for (int i = 0; i < peer_count; i++) {
            if (peers[i].fd == fd) {
                snprintf(peers[i].name, MAX_NAME, "%.*s",
                         hdr->payload_len < MAX_NAME ? hdr->payload_len : MAX_NAME - 1,
                         payload);
                util_log(LOG_INFO, "server: peer fd=%d identified as \"%s\"", fd, peers[i].name);
                break;
            }
        }
        pthread_mutex_unlock(&peer_lock);
        break;
    }

    case MSG_CHAT: {
        /* payload format: "recipient\0message" */
        const char *sep = memchr(payload, '\0', hdr->payload_len);
        if (!sep) break;

        const char *to  = payload;
        const char *msg = sep + 1;
        int msg_len = hdr->payload_len - (int)(msg - payload);

        /* Find sender name */
        char sender[MAX_NAME] = "unknown";
        pthread_mutex_lock(&peer_lock);
        for (int i = 0; i < peer_count; i++)
            if (peers[i].fd == fd) { snprintf(sender, MAX_NAME, "%s", peers[i].name); break; }
        pthread_mutex_unlock(&peer_lock);

        /* Build routed packet: "sender\0message" */
        char route_buf[MAX_MSG + MAX_NAME + sizeof(PktHeader)];
        int route_payload_len = (int)strlen(sender) + 1 + msg_len;

        PktHeader rh;
        rh.type        = MSG_CHAT;
        rh.seq         = hdr->seq;
        rh.payload_len = (uint16_t)route_payload_len;

        memcpy(route_buf, &rh, sizeof(PktHeader));
        memcpy(route_buf + sizeof(PktHeader), sender, strlen(sender) + 1);
        memcpy(route_buf + sizeof(PktHeader) + strlen(sender) + 1, msg, msg_len);

        pthread_mutex_lock(&peer_lock);
        Peer *target = peer_find_by_name(to);
        if (target) {
            send_all(target->fd, route_buf, sizeof(PktHeader) + route_payload_len);
        } else {
            broadcast_to_all(route_buf, sizeof(PktHeader) + route_payload_len, fd);
        }
        pthread_mutex_unlock(&peer_lock);

        util_log(LOG_INFO, "server: chat from \"%s\" to \"%s\" (%d bytes)", sender, to, msg_len);
        break;
    }

    case MSG_FILE_META:
    case MSG_FILE_CHUNK:
    case MSG_FILE_ACK:
    case MSG_FILE_NACK:
    case MSG_PAUSE:
    case MSG_RESUME: {
        /* File messages: payload starts with "recipient\0..." for META,
         * or xfer_id(4B) for CHUNK/ACK/NACK/PAUSE/RESUME.
         * Route the entire packet (header + payload) to the target peer. */
        const char *to = NULL;
        if (hdr->type == MSG_FILE_META) {
            to = payload;
        } else {
            /* For chunk/ack/nack/pause/resume, we forward to all other peers
             * since the xfer_id identifies the transfer on both sides */
            char fwd_buf[CHUNK_SIZE + 256 + sizeof(PktHeader)];
            memcpy(fwd_buf, hdr, sizeof(PktHeader));
            memcpy(fwd_buf + sizeof(PktHeader), payload, hdr->payload_len);
            broadcast_to_all(fwd_buf, sizeof(PktHeader) + hdr->payload_len, fd);
            break;
        }

        /* Route META to target peer */
        char fwd_buf[MAX_MSG + sizeof(PktHeader)];
        memcpy(fwd_buf, hdr, sizeof(PktHeader));
        memcpy(fwd_buf + sizeof(PktHeader), payload, hdr->payload_len);

        pthread_mutex_lock(&peer_lock);
        Peer *target = peer_find_by_name(to);
        if (target) {
            send_all(target->fd, fwd_buf, sizeof(PktHeader) + hdr->payload_len);
        } else {
            broadcast_to_all(fwd_buf, sizeof(PktHeader) + hdr->payload_len, fd);
        }
        pthread_mutex_unlock(&peer_lock);
        break;
    }

    case MSG_BYE:
        peer_remove(fd);
        break;

    default:
        util_log(LOG_WARN, "server: unknown msg type 0x%02x from fd=%d", hdr->type, fd);
        break;
    }
}

static void *server_loop(void *arg)
{
    (void)arg;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { util_log(LOG_ERROR, "server socket: %s", strerror(errno)); return NULL; }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DATA_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        util_log(LOG_ERROR, "server bind: %s", strerror(errno));
        close(listen_fd); listen_fd = -1;
        return NULL;
    }

    if (listen(listen_fd, 8) < 0) {
        util_log(LOG_ERROR, "server listen: %s", strerror(errno));
        close(listen_fd); listen_fd = -1;
        return NULL;
    }

    util_log(LOG_INFO, "server: listening on port %d as \"%s\"", DATA_PORT, server_name);
    discovery_start_announce(server_name, DATA_PORT);

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        pthread_mutex_lock(&peer_lock);
        for (int i = 0; i < peer_count; i++) {
            FD_SET(peers[i].fd, &rfds);
            if (peers[i].fd > maxfd) maxfd = peers[i].fd;
        }
        pthread_mutex_unlock(&peer_lock);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (ready == 0) continue;

        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int cfd = accept(listen_fd, (struct sockaddr *)&cli, &clen);
            if (cfd >= 0) {
                char ip[46];
                inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
                util_log(LOG_INFO, "server: new connection from %s:%d (fd=%d)", ip, ntohs(cli.sin_port), cfd);
                peer_add(cfd, ip, ntohs(cli.sin_port));
            }
        }

        pthread_mutex_lock(&peer_lock);
        int snapshot_count = peer_count;
        int snapshot_fds[MAX_PEERS];
        for (int i = 0; i < snapshot_count; i++)
            snapshot_fds[i] = peers[i].fd;
        pthread_mutex_unlock(&peer_lock);

        for (int i = 0; i < snapshot_count; i++) {
            int fd = snapshot_fds[i];
            if (!FD_ISSET(fd, &rfds)) continue;

            PktHeader hdr;
            ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
            if (n <= 0) { peer_remove(fd); continue; }

            char payload[CHUNK_SIZE + 256];
            int max_payload = (int)sizeof(payload);
            if (hdr.payload_len > 0 && hdr.payload_len <= max_payload) {
                n = recv(fd, payload, hdr.payload_len, MSG_WAITALL);
                if (n <= 0) { peer_remove(fd); continue; }
            } else if (hdr.payload_len > 0) {
                peer_remove(fd); continue;
            }

            handle_packet(fd, &hdr, payload);
        }
    }

    discovery_stop_announce();

    pthread_mutex_lock(&peer_lock);
    for (int i = 0; i < peer_count; i++)
        close(peers[i].fd);
    peer_count = 0;
    pthread_mutex_unlock(&peer_lock);

    close(listen_fd);
    listen_fd = -1;
    return NULL;
}

void server_start(const char *name)
{
    if (running) return;
    running = 1;
    snprintf(server_name, MAX_NAME, "%s", name);
    pthread_create(&server_thread, NULL, server_loop, NULL);
}

void server_stop(void)
{
    if (!running) return;
    running = 0;
    pthread_join(server_thread, NULL);
}

int server_get_peers(Peer *out, int max)
{
    int count;
    pthread_mutex_lock(&peer_lock);
    count = peer_count < max ? peer_count : max;
    memcpy(out, peers, count * sizeof(Peer));
    pthread_mutex_unlock(&peer_lock);
    return count;
}

int server_is_running(void)
{
    return running;
}
