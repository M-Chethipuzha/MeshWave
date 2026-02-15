/* client.c
 * TCP client: connect to chosen server, send chat, receive events.
 * Incoming packets are pushed to a ring buffer that the HTTP layer drains.
 */

#include "client.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

static int            sock_fd     = -1;
static volatile int   connected   = 0;
static pthread_t      recv_thread;
static char           username[MAX_NAME];

static ChatEvent      event_queue[EVENT_QUEUE_SIZE];
static int            eq_head = 0;
static int            eq_tail = 0;
static pthread_mutex_t eq_lock = PTHREAD_MUTEX_INITIALIZER;

static void event_push(const ChatEvent *ev)
{
    pthread_mutex_lock(&eq_lock);
    event_queue[eq_head] = *ev;
    eq_head = (eq_head + 1) % EVENT_QUEUE_SIZE;
    if (eq_head == eq_tail)
        eq_tail = (eq_tail + 1) % EVENT_QUEUE_SIZE;
    pthread_mutex_unlock(&eq_lock);
}

int client_poll_event(ChatEvent *out)
{
    int got = 0;
    pthread_mutex_lock(&eq_lock);
    if (eq_tail != eq_head) {
        *out = event_queue[eq_tail];
        eq_tail = (eq_tail + 1) % EVENT_QUEUE_SIZE;
        got = 1;
    }
    pthread_mutex_unlock(&eq_lock);
    return got;
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

static void *recv_loop(void *arg)
{
    (void)arg;

    while (connected) {
        PktHeader hdr;
        ssize_t n = recv(sock_fd, &hdr, sizeof(hdr), MSG_WAITALL);
        if (n <= 0) {
            util_log(LOG_WARN, "client: server disconnected");
            connected = 0;
            break;
        }

        char payload[MAX_MSG];
        if (hdr.payload_len > 0 && hdr.payload_len <= MAX_MSG) {
            n = recv(sock_fd, payload, hdr.payload_len, MSG_WAITALL);
            if (n <= 0) { connected = 0; break; }
        } else {
            continue;
        }

        if (hdr.type == MSG_CHAT) {
            const char *sep = memchr(payload, '\0', hdr.payload_len);
            if (!sep) continue;

            ChatEvent ev;
            memset(&ev, 0, sizeof(ev));
            snprintf(ev.from, MAX_NAME, "%s", payload);
            int msg_len = hdr.payload_len - (int)(sep - payload) - 1;
            if (msg_len > 0 && msg_len < MAX_MSG)
                memcpy(ev.text, sep + 1, msg_len);
            ev.timestamp = util_time_ms();

            event_push(&ev);
            util_log(LOG_INFO, "client: chat from \"%s\": %s", ev.from, ev.text);
        }
    }

    return NULL;
}

int client_connect(const char *ip, uint16_t port, const char *user)
{
    if (connected) return -1;

    snprintf(username, MAX_NAME, "%s", user);

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { util_log(LOG_ERROR, "client socket: %s", strerror(errno)); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        util_log(LOG_ERROR, "client: invalid IP \"%s\"", ip);
        close(sock_fd); sock_fd = -1;
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        util_log(LOG_ERROR, "client connect: %s", strerror(errno));
        close(sock_fd); sock_fd = -1;
        return -1;
    }

    PktHeader hdr;
    hdr.type        = MSG_HELLO;
    hdr.seq         = 0;
    hdr.payload_len = (uint16_t)strlen(username);

    if (send_all(sock_fd, &hdr, sizeof(hdr)) < 0 ||
        send_all(sock_fd, username, strlen(username)) < 0) {
        util_log(LOG_ERROR, "client: hello send failed");
        close(sock_fd); sock_fd = -1;
        return -1;
    }

    connected = 1;
    util_log(LOG_INFO, "client: connected to %s:%d as \"%s\"", ip, port, username);

    pthread_create(&recv_thread, NULL, recv_loop, NULL);
    return 0;
}

void client_disconnect(void)
{
    if (!connected) return;

    PktHeader hdr;
    hdr.type        = MSG_BYE;
    hdr.seq         = 0;
    hdr.payload_len = 0;
    send_all(sock_fd, &hdr, sizeof(hdr));

    connected = 0;
    pthread_join(recv_thread, NULL);
    close(sock_fd);
    sock_fd = -1;
    util_log(LOG_INFO, "client: disconnected");
}

int client_send_chat(const char *to, const char *text)
{
    if (!connected) return -1;

    int to_len  = (int)strlen(to);
    int txt_len = (int)strlen(text);
    int total   = to_len + 1 + txt_len;

    if (total > MAX_MSG) return -1;

    char payload[MAX_MSG];
    memcpy(payload, to, to_len + 1);
    memcpy(payload + to_len + 1, text, txt_len);

    PktHeader hdr;
    hdr.type        = MSG_CHAT;
    hdr.seq         = 0;
    hdr.payload_len = (uint16_t)total;

    if (send_all(sock_fd, &hdr, sizeof(hdr)) < 0 ||
        send_all(sock_fd, payload, total) < 0)
        return -1;

    return 0;
}

int client_is_connected(void)
{
    return connected;
}

const char *client_get_username(void)
{
    return username;
}
