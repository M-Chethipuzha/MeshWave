/* transfer.c
 * Chunked file send and receive.
 * Handles pause, resume, and per-chunk retry on NACK.
 */

#include "transfer.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

static Transfer       transfers[MAX_TRANSFERS];
static int            xfer_count = 0;
static int            id_counter = 1;
static pthread_mutex_t xfer_lock = PTHREAD_MUTEX_INITIALIZER;
static TransferEventCb event_cb  = NULL;

/* File receive state (receiver keeps open file handles) */
typedef struct {
    int   xfer_id;
    FILE *fp;
    char  path[512];
    uint64_t file_size;
    uint64_t received_bytes;
} RecvCtx;

static RecvCtx recv_ctxs[MAX_TRANSFERS];
static int     recv_count = 0;

static void notify(int xfer_id, XferState state, uint32_t done, uint32_t total)
{
    if (event_cb) event_cb(xfer_id, state, done, total);
}

void transfer_init(TransferEventCb cb)
{
    event_cb = cb;
    memset(transfers, 0, sizeof(transfers));
    memset(recv_ctxs, 0, sizeof(recv_ctxs));
    xfer_count = 0;
    recv_count = 0;
    id_counter = 1;
}

int transfer_next_id(void)
{
    int id;
    pthread_mutex_lock(&xfer_lock);
    id = id_counter++;
    pthread_mutex_unlock(&xfer_lock);
    return id;
}

static Transfer *alloc_transfer(void)
{
    pthread_mutex_lock(&xfer_lock);
    Transfer *t = NULL;
    if (xfer_count < MAX_TRANSFERS) {
        t = &transfers[xfer_count++];
        memset(t, 0, sizeof(Transfer));
    }
    pthread_mutex_unlock(&xfer_lock);
    return t;
}

static RecvCtx *find_recv_ctx(int xfer_id)
{
    for (int i = 0; i < recv_count; i++)
        if (recv_ctxs[i].xfer_id == xfer_id)
            return &recv_ctxs[i];
    return NULL;
}

Transfer *transfer_find(int xfer_id)
{
    pthread_mutex_lock(&xfer_lock);
    for (int i = 0; i < xfer_count; i++) {
        if (transfers[i].id == xfer_id) {
            pthread_mutex_unlock(&xfer_lock);
            return &transfers[i];
        }
    }
    pthread_mutex_unlock(&xfer_lock);
    return NULL;
}

int transfer_get_all(Transfer *out, int max)
{
    int count;
    pthread_mutex_lock(&xfer_lock);
    count = xfer_count < max ? xfer_count : max;
    memcpy(out, transfers, count * sizeof(Transfer));
    pthread_mutex_unlock(&xfer_lock);
    return count;
}

/* ── Sending (runs in its own thread per transfer) ─────── */

typedef struct {
    int     xfer_id;
    int     sock_fd;
    char    filepath[512];
    char    peer[MAX_NAME];
} SendCtx;

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

static int wait_for_ack(int fd, uint32_t expected_seq, int timeout_s)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };

    int ready = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0) return -1;

    PktHeader hdr;
    ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (n <= 0) return -1;

    /* Drain payload if any */
    if (hdr.payload_len > 0) {
        char tmp[64];
        int left = hdr.payload_len;
        while (left > 0) {
            int chunk = left < (int)sizeof(tmp) ? left : (int)sizeof(tmp);
            n = recv(fd, tmp, chunk, MSG_WAITALL);
            if (n <= 0) break;
            left -= (int)n;
        }
    }

    if (hdr.type == MSG_FILE_ACK && hdr.seq == expected_seq)
        return 0;
    if (hdr.type == MSG_FILE_NACK)
        return 1;
    if (hdr.type == MSG_PAUSE)
        return 2;

    return -1;
}

static void *send_thread(void *arg)
{
    SendCtx *ctx = (SendCtx *)arg;
    Transfer *t = transfer_find(ctx->xfer_id);
    if (!t) { free(ctx); return NULL; }

    FILE *fp = fopen(ctx->filepath, "rb");
    if (!fp) {
        util_log(LOG_ERROR, "transfer: cannot open %s: %s", ctx->filepath, strerror(errno));
        t->state = XFER_ERROR;
        notify(t->id, XFER_ERROR, 0, t->total_chunks);
        free(ctx);
        return NULL;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    t->total_chunks = (uint32_t)((file_size + CHUNK_SIZE - 1) / CHUNK_SIZE);
    t->state = XFER_ACTIVE;

    /* Send META: "peer\0filename\0total_chunks(4B)\0file_size(8B)" */
    {
        const char *basename = strrchr(ctx->filepath, '/');
        basename = basename ? basename + 1 : ctx->filepath;

        int peer_len = (int)strlen(ctx->peer);
        int name_len = (int)strlen(basename);
        int payload_len = peer_len + 1 + name_len + 1 + 4 + 8;

        char meta_payload[512];
        char *p = meta_payload;
        memcpy(p, ctx->peer, peer_len + 1); p += peer_len + 1;
        memcpy(p, basename, name_len + 1);  p += name_len + 1;

        uint32_t tc_net = htonl(t->total_chunks);
        memcpy(p, &tc_net, 4); p += 4;

        uint64_t fs_val = (uint64_t)file_size;
        /* Store file_size in big-endian */
        for (int i = 7; i >= 0; i--) {
            p[i] = (char)(fs_val & 0xFF);
            fs_val >>= 8;
        }
        p += 8;

        PktHeader mhdr;
        mhdr.type = MSG_FILE_META;
        mhdr.seq  = 0;
        mhdr.payload_len = (uint16_t)payload_len;

        send_all(ctx->sock_fd, &mhdr, sizeof(mhdr));
        send_all(ctx->sock_fd, meta_payload, payload_len);
    }

    notify(t->id, XFER_ACTIVE, 0, t->total_chunks);
    util_log(LOG_INFO, "transfer: sending \"%s\" (%ld bytes, %u chunks) to \"%s\"",
             ctx->filepath, file_size, t->total_chunks, ctx->peer);

    uint8_t *chunk_buf = (uint8_t *)malloc(CHUNK_SIZE);
    if (!chunk_buf) { fclose(fp); t->state = XFER_ERROR; free(ctx); return NULL; }

    for (uint32_t seq = 0; seq < t->total_chunks; seq++) {
        /* Check for pause */
        while (t->state == XFER_PAUSED) {
            usleep(200000);
            if (t->state == XFER_ERROR) break;
        }
        if (t->state == XFER_ERROR) break;

        /* Skip already-acked chunks (for resume from bitmask) */
        if (t->chunk_map && (t->chunk_map[seq / 8] & (1 << (seq % 8))))
            continue;

        fseek(fp, (long)seq * CHUNK_SIZE, SEEK_SET);
        size_t bytes_read = fread(chunk_buf, 1, CHUNK_SIZE, fp);
        if (bytes_read == 0) break;

        int retries = 0;
        int acked = 0;

        while (retries < XFER_MAX_RETRIES && !acked) {
            PktHeader chdr;
            chdr.type        = MSG_FILE_CHUNK;
            chdr.seq         = seq;
            chdr.payload_len = (uint16_t)bytes_read;

            /* Prepend xfer_id (4 bytes) to chunk payload */
            uint32_t id_net = htonl((uint32_t)t->id);
            PktHeader fhdr;
            fhdr.type = MSG_FILE_CHUNK;
            fhdr.seq  = seq;
            fhdr.payload_len = (uint16_t)(4 + bytes_read);

            if (send_all(ctx->sock_fd, &fhdr, sizeof(fhdr)) < 0 ||
                send_all(ctx->sock_fd, &id_net, 4) < 0 ||
                send_all(ctx->sock_fd, chunk_buf, (int)bytes_read) < 0) {
                t->state = XFER_ERROR;
                break;
            }

            int result = wait_for_ack(ctx->sock_fd, seq, XFER_TIMEOUT_S);
            if (result == 0) {
                acked = 1;
                t->done_chunks = seq + 1;
                notify(t->id, XFER_ACTIVE, t->done_chunks, t->total_chunks);
            } else if (result == 2) {
                /* Pause requested */
                t->state = XFER_PAUSED;
                notify(t->id, XFER_PAUSED, t->done_chunks, t->total_chunks);
                util_log(LOG_INFO, "transfer %d: paused at chunk %u", t->id, seq);
                seq--;
                break;
            } else {
                retries++;
                util_log(LOG_WARN, "transfer %d: chunk %u retry %d/%d",
                         t->id, seq, retries, XFER_MAX_RETRIES);
            }
        }

        if (!acked && t->state != XFER_PAUSED) {
            t->state = XFER_ERROR;
            notify(t->id, XFER_ERROR, t->done_chunks, t->total_chunks);
            util_log(LOG_ERROR, "transfer %d: failed at chunk %u after %d retries",
                     t->id, seq, XFER_MAX_RETRIES);
            break;
        }
    }

    if (t->state == XFER_ACTIVE && t->done_chunks == t->total_chunks) {
        t->state = XFER_DONE;
        notify(t->id, XFER_DONE, t->done_chunks, t->total_chunks);
        util_log(LOG_INFO, "transfer %d: complete", t->id);
    }

    free(chunk_buf);
    fclose(fp);
    free(ctx);
    return NULL;
}

int transfer_send_file(int sock_fd, const char *filepath, const char *peer_name)
{
    Transfer *t = alloc_transfer();
    if (!t) return -1;

    t->id    = transfer_next_id();
    t->state = XFER_IDLE;
    snprintf(t->filename, 256, "%s", filepath);
    snprintf(t->peer, MAX_NAME, "%s", peer_name);

    SendCtx *ctx = (SendCtx *)malloc(sizeof(SendCtx));
    ctx->xfer_id = t->id;
    ctx->sock_fd = sock_fd;
    snprintf(ctx->filepath, 512, "%s", filepath);
    snprintf(ctx->peer, MAX_NAME, "%s", peer_name);

    pthread_t tid;
    pthread_create(&tid, NULL, send_thread, ctx);
    pthread_detach(tid);

    return t->id;
}

/* ── Receiving ─────────────────────────────────────────── */

int transfer_recv_meta(int xfer_id, const char *sender,
                       const char *filename, uint32_t total_chunks,
                       uint64_t file_size, const char *save_dir)
{
    Transfer *t = alloc_transfer();
    if (!t) return -1;

    t->id           = xfer_id;
    t->state        = XFER_ACTIVE;
    t->total_chunks = total_chunks;
    t->done_chunks  = 0;
    snprintf(t->filename, 256, "%s", filename);
    snprintf(t->peer, MAX_NAME, "%s", sender);

    int map_size = (total_chunks + 7) / 8;
    t->chunk_map = (uint8_t *)calloc(1, map_size);

    /* Set up receive context */
    RecvCtx *rc = &recv_ctxs[recv_count++];
    rc->xfer_id = xfer_id;
    rc->file_size = file_size;
    rc->received_bytes = 0;

    char path[512];
    if (save_dir && save_dir[0])
        snprintf(path, sizeof(path), "%s/%s", save_dir, filename);
    else
        snprintf(path, sizeof(path), "%s", filename);

    snprintf(rc->path, sizeof(rc->path), "%s", path);

    rc->fp = fopen(path, "wb");
    if (!rc->fp) {
        util_log(LOG_ERROR, "transfer: cannot create %s: %s", path, strerror(errno));
        t->state = XFER_ERROR;
        return -1;
    }

    /* Pre-allocate file */
    if (file_size > 0) {
        fseek(rc->fp, (long)(file_size - 1), SEEK_SET);
        fputc(0, rc->fp);
        fseek(rc->fp, 0, SEEK_SET);
    }

    notify(t->id, XFER_ACTIVE, 0, total_chunks);
    util_log(LOG_INFO, "transfer: receiving \"%s\" from \"%s\" (%u chunks, %llu bytes)",
             filename, sender, total_chunks, (unsigned long long)file_size);

    return 0;
}

int transfer_recv_chunk(int xfer_id, uint32_t chunk_seq,
                        const uint8_t *data, int data_len)
{
    Transfer *t = transfer_find(xfer_id);
    if (!t) return -1;

    if (t->state == XFER_PAUSED || t->state == XFER_ERROR)
        return -1;

    RecvCtx *rc = find_recv_ctx(xfer_id);
    if (!rc || !rc->fp) return -1;

    /* Write chunk to correct offset */
    long offset = (long)chunk_seq * CHUNK_SIZE;
    fseek(rc->fp, offset, SEEK_SET);
    size_t written = fwrite(data, 1, data_len, rc->fp);
    fflush(rc->fp);

    if ((int)written != data_len) {
        util_log(LOG_ERROR, "transfer %d: write error at chunk %u", xfer_id, chunk_seq);
        return -1;
    }

    /* Mark chunk in bitmask */
    if (t->chunk_map)
        t->chunk_map[chunk_seq / 8] |= (1 << (chunk_seq % 8));

    t->done_chunks++;
    rc->received_bytes += data_len;

    notify(t->id, XFER_ACTIVE, t->done_chunks, t->total_chunks);

    /* Check if complete */
    if (t->done_chunks >= t->total_chunks) {
        t->state = XFER_DONE;
        fclose(rc->fp);
        rc->fp = NULL;
        notify(t->id, XFER_DONE, t->done_chunks, t->total_chunks);
        util_log(LOG_INFO, "transfer %d: receive complete -> %s", xfer_id, rc->path);
    }

    return 0;
}

/* ── Pause / Resume ────────────────────────────────────── */

int transfer_pause(int xfer_id)
{
    Transfer *t = transfer_find(xfer_id);
    if (!t || t->state != XFER_ACTIVE) return -1;

    t->state = XFER_PAUSED;
    notify(t->id, XFER_PAUSED, t->done_chunks, t->total_chunks);
    util_log(LOG_INFO, "transfer %d: paused", xfer_id);
    return 0;
}

int transfer_resume(int xfer_id)
{
    Transfer *t = transfer_find(xfer_id);
    if (!t || t->state != XFER_PAUSED) return -1;

    t->state = XFER_ACTIVE;
    notify(t->id, XFER_ACTIVE, t->done_chunks, t->total_chunks);
    util_log(LOG_INFO, "transfer %d: resumed", xfer_id);
    return 0;
}
