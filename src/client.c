/* client.c
 * TCP client: connect to chosen server, send chat and files, receive events.
 * Incoming packets are pushed to a ring buffer that the HTTP layer drains.
 */

#include "client.h"
#include "transfer.h"
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

        char payload[CHUNK_SIZE + 256];
        int max_payload = (int)sizeof(payload);
        if (hdr.payload_len > 0 && hdr.payload_len <= max_payload) {
            n = recv(sock_fd, payload, hdr.payload_len, MSG_WAITALL);
            if (n <= 0) { connected = 0; break; }
        } else if (hdr.payload_len > 0) {
            continue;
        } else {
            continue;
        }

        if (hdr.type == MSG_CHAT) {
            const char *sep = memchr(payload, '\0', hdr.payload_len);
            if (!sep) continue;

            ChatEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EVT_CHAT;
            snprintf(ev.from, MAX_NAME, "%s", payload);
            int msg_len = hdr.payload_len - (int)(sep - payload) - 1;
            if (msg_len > 0 && msg_len < MAX_MSG)
                memcpy(ev.text, sep + 1, msg_len);
            ev.timestamp = util_time_ms();

            event_push(&ev);
            util_log(LOG_INFO, "client: chat from \"%s\": %s", ev.from, ev.text);
        }
        else if (hdr.type == MSG_FILE_META) {
            /* payload: "recipient\0filename\0total_chunks(4B)file_size(8B)" */
            const char *sep1 = memchr(payload, '\0', hdr.payload_len);
            if (!sep1) continue;

            const char *filename = sep1 + 1;
            const char *sep2 = memchr(filename, '\0', hdr.payload_len - (int)(filename - payload));
            if (!sep2) continue;

            const char *bin = sep2 + 1;
            uint32_t total_chunks = ntohl(*(uint32_t *)bin);
            bin += 4;

            uint64_t file_size = 0;
            for (int i = 0; i < 8; i++)
                file_size = (file_size << 8) | (uint8_t)bin[i];

            int xfer_id = transfer_next_id();
            transfer_recv_meta(xfer_id, "sender", filename, total_chunks, file_size, "./downloads");

            /* Send ACK for meta */
            PktHeader ack;
            ack.type = MSG_FILE_ACK;
            ack.seq  = 0;
            ack.payload_len = 0;
            send_all(sock_fd, &ack, sizeof(ack));

            util_log(LOG_INFO, "client: incoming file \"%s\" (%u chunks)", filename, total_chunks);
        }
        else if (hdr.type == MSG_FILE_CHUNK) {
            /* payload: xfer_id(4B) + chunk_data */
            if (hdr.payload_len < 4) continue;

            uint32_t xfer_id = ntohl(*(uint32_t *)payload);
            const uint8_t *chunk_data = (const uint8_t *)(payload + 4);
            int chunk_len = hdr.payload_len - 4;

            int rc = transfer_recv_chunk((int)xfer_id, hdr.seq, chunk_data, chunk_len);

            PktHeader ack;
            ack.type = (rc == 0) ? MSG_FILE_ACK : MSG_FILE_NACK;
            ack.seq  = hdr.seq;
            ack.payload_len = 0;
            send_all(sock_fd, &ack, sizeof(ack));

            Transfer *t = transfer_find((int)xfer_id);
            if (t) {
                ChatEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.xfer_id = t->id;
                ev.done_chunks = t->done_chunks;
                ev.total_chunks = t->total_chunks;
                ev.xfer_state = t->state;
                snprintf(ev.filename, 256, "%s", t->filename);
                snprintf(ev.from, MAX_NAME, "%s", t->peer);
                ev.timestamp = util_time_ms();

                if (t->state == XFER_DONE) {
                    ev.type = EVT_FILE_COMPLETE;
                } else if (t->state == XFER_ERROR) {
                    ev.type = EVT_FILE_ERROR;
                } else {
                    ev.type = EVT_FILE_PROGRESS;
                }
                event_push(&ev);
            }
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

int client_send_file(const char *filepath, const char *to)
{
    if (!connected) return -1;
    return transfer_send_file(sock_fd, filepath, to);
}

int client_pause_transfer(int xfer_id)
{
    return transfer_pause(xfer_id);
}

int client_resume_transfer(int xfer_id)
{
    return transfer_resume(xfer_id);
}

int client_is_connected(void)
{
    return connected;
}

const char *client_get_username(void)
{
    return username;
}

int client_get_sock_fd(void)
{
    return sock_fd;
}
