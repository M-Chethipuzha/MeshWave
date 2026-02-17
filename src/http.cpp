/* http.cpp
 * Minimal embedded HTTP/1.1 server.
 * Serves index.html from web_bundle.h, provides REST API and SSE events.
 */

#include "http.h"
#include "protocol.h"
#include "web_bundle.h"

extern "C" {
#include "discovery.h"
#include "server.h"
#include "client.h"
#include "transfer.h"
#include "util.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>

static int            http_fd   = -1;
static pthread_t      http_thread;
static volatile int   http_running = 0;

/* SSE client tracking */
#define MAX_SSE 16
static int            sse_fds[MAX_SSE];
static int            sse_count = 0;
static pthread_mutex_t sse_lock = PTHREAD_MUTEX_INITIALIZER;

static void sse_add(int fd)
{
    pthread_mutex_lock(&sse_lock);
    if (sse_count < MAX_SSE)
        sse_fds[sse_count++] = fd;
    else
        close(fd);
    pthread_mutex_unlock(&sse_lock);
}

static void sse_broadcast(const char *event, const char *data)
{
    char buf[MAX_MSG + 256];
    int len = snprintf(buf, sizeof(buf), "event: %s\ndata: %s\n\n", event, data);

    pthread_mutex_lock(&sse_lock);
    for (int i = 0; i < sse_count; ) {
        ssize_t n = send(sse_fds[i], buf, len, MSG_NOSIGNAL);
        if (n <= 0) {
            close(sse_fds[i]);
            sse_fds[i] = sse_fds[--sse_count];
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&sse_lock);
}

/* ── tiny JSON helpers ──────────────────────────────────── */

static std::string json_escape(const char *s)
{
    std::string out;
    for (; *s; s++) {
        switch (*s) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += *s;     break;
        }
    }
    return out;
}

/* ── HTTP parsing ───────────────────────────────────────── */

struct HttpReq {
    char method[16];
    char path[256];
    char body[MAX_MSG];
    int  body_len;
    int  content_length;
};

static int parse_request(int fd, HttpReq *req)
{
    char raw[8192];
    memset(raw, 0, sizeof(raw));
    memset(req, 0, sizeof(*req));

    ssize_t total = 0;
    while (total < (ssize_t)sizeof(raw) - 1) {
        ssize_t n = recv(fd, raw + total, sizeof(raw) - 1 - total, 0);
        if (n <= 0) return -1;
        total += n;
        if (strstr(raw, "\r\n\r\n")) break;
    }

    sscanf(raw, "%15s %255s", req->method, req->path);

    char *cl = strcasestr(raw, "Content-Length:");
    if (cl) req->content_length = atoi(cl + 15);

    char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        int already = (int)(total - (body_start - raw));
        memcpy(req->body, body_start, already);

        while (already < req->content_length && already < MAX_MSG - 1) {
            ssize_t n = recv(fd, req->body + already, req->content_length - already, 0);
            if (n <= 0) break;
            already += (int)n;
        }
        req->body_len = already;
        req->body[already] = '\0';
    }

    return 0;
}

/* ── HTTP response helpers ──────────────────────────────── */

static void send_response(int fd, int code, const char *ctype, const char *body, int blen)
{
    char hdr[512];
    const char *status = code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request";
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status, ctype, blen);

    send(fd, hdr, hlen, 0);
    if (blen > 0) send(fd, body, blen, 0);
}

static void send_json(int fd, int code, const std::string &json)
{
    send_response(fd, code, "application/json", json.c_str(), (int)json.size());
}

static void send_sse_headers(int fd)
{
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    send(fd, hdr, strlen(hdr), 0);
}

/* ── tiny JSON field extractor ──────────────────────────── */

static std::string json_field(const char *json, const char *key)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return "";
    p += strlen(needle);
    while (*p == ' ') p++;

    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (!e) return "";
        return std::string(p, e - p);
    }

    const char *e = p;
    while (*e && *e != ',' && *e != '}' && *e != ' ') e++;
    return std::string(p, e - p);
}

/* ── Route handling ─────────────────────────────────────── */

static void handle_request(int fd, HttpReq *req)
{
    /* GET / — serve dashboard */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/") == 0) {
        send_response(fd, 200, "text/html", index_html, (int)strlen(index_html));
        close(fd);
        return;
    }

    /* GET /api/servers — discovered servers */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/servers") == 0) {
        ServerInfo svs[MAX_PEERS];
        int n = discovery_get_servers(svs, MAX_PEERS);

        std::string json = "[";
        for (int i = 0; i < n; i++) {
            if (i) json += ",";
            json += "{\"name\":\"" + json_escape(svs[i].name) +
                    "\",\"ip\":\"" + svs[i].ip +
                    "\",\"port\":" + std::to_string(svs[i].port) + "}";
        }
        json += "]";
        send_json(fd, 200, json);
        close(fd);
        return;
    }

    /* GET /api/peers — connected peers (server mode) */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/peers") == 0) {
        Peer ps[MAX_PEERS];
        int n = server_get_peers(ps, MAX_PEERS);

        std::string json = "[";
        for (int i = 0; i < n; i++) {
            if (i) json += ",";
            json += "{\"name\":\"" + json_escape(ps[i].name) +
                    "\",\"addr\":\"" + ps[i].addr +
                    "\",\"port\":" + std::to_string(ps[i].port) + "}";
        }
        json += "]";
        send_json(fd, 200, json);
        close(fd);
        return;
    }

    /* GET /api/status — current mode, username, connection info */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/status") == 0) {
        std::string json = "{";
        json += "\"server_running\":" + std::string(server_is_running() ? "true" : "false");
        json += ",\"client_connected\":" + std::string(client_is_connected() ? "true" : "false");
        json += ",\"username\":\"" + json_escape(client_get_username()) + "\"";
        json += "}";
        send_json(fd, 200, json);
        close(fd);
        return;
    }

    /* POST /api/mode — set mode {mode: "server"|"client", name: "...", ip: "...", port: N} */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/mode") == 0) {
        std::string mode = json_field(req->body, "mode");
        std::string name = json_field(req->body, "name");

        if (mode == "server") {
            if (name.empty()) name = "MeshWave-Server";
            server_start(name.c_str());
            send_json(fd, 200, "{\"ok\":true,\"mode\":\"server\"}");
        } else if (mode == "client") {
            std::string ip   = json_field(req->body, "ip");
            std::string port = json_field(req->body, "port");
            if (name.empty()) name = "User";
            if (ip.empty() || port.empty()) {
                send_json(fd, 400, "{\"error\":\"ip and port required\"}");
                close(fd);
                return;
            }
            int rc = client_connect(ip.c_str(), (uint16_t)atoi(port.c_str()), name.c_str());
            if (rc == 0)
                send_json(fd, 200, "{\"ok\":true,\"mode\":\"client\"}");
            else
                send_json(fd, 400, "{\"error\":\"connection failed\"}");
        } else {
            send_json(fd, 400, "{\"error\":\"mode must be server or client\"}");
        }
        close(fd);
        return;
    }

    /* POST /api/chat — send a chat message {to, text} */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/chat") == 0) {
        std::string to   = json_field(req->body, "to");
        std::string text = json_field(req->body, "text");

        if (to.empty() || text.empty()) {
            send_json(fd, 400, "{\"error\":\"to and text required\"}");
            close(fd);
            return;
        }

        int rc = client_send_chat(to.c_str(), text.c_str());
        send_json(fd, rc == 0 ? 200 : 400,
                  rc == 0 ? "{\"ok\":true}" : "{\"error\":\"send failed\"}");
        close(fd);
        return;
    }

    /* GET /api/events — SSE stream */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/events") == 0) {
        send_sse_headers(fd);
        sse_add(fd);
        return; /* fd stays open, managed by SSE system */
    }

    /* POST /api/file/send — initiate file transfer {path, to} */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/file/send") == 0) {
        std::string path = json_field(req->body, "path");
        std::string to   = json_field(req->body, "to");

        if (path.empty() || to.empty()) {
            send_json(fd, 400, "{\"error\":\"path and to required\"}");
            close(fd);
            return;
        }

        int xfer_id = client_send_file(path.c_str(), to.c_str());
        if (xfer_id >= 0) {
            send_json(fd, 200, "{\"ok\":true,\"id\":" + std::to_string(xfer_id) + "}");
        } else {
            send_json(fd, 400, "{\"error\":\"send failed\"}");
        }
        close(fd);
        return;
    }

    /* POST /api/file/pause — pause transfer {id} */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/file/pause") == 0) {
        std::string id_str = json_field(req->body, "id");
        if (id_str.empty()) {
            send_json(fd, 400, "{\"error\":\"id required\"}");
            close(fd);
            return;
        }
        int rc = client_pause_transfer(atoi(id_str.c_str()));
        send_json(fd, rc == 0 ? 200 : 400,
                  rc == 0 ? "{\"ok\":true}" : "{\"error\":\"pause failed\"}");
        close(fd);
        return;
    }

    /* POST /api/file/resume — resume transfer {id} */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/file/resume") == 0) {
        std::string id_str = json_field(req->body, "id");
        if (id_str.empty()) {
            send_json(fd, 400, "{\"error\":\"id required\"}");
            close(fd);
            return;
        }
        int rc = client_resume_transfer(atoi(id_str.c_str()));
        send_json(fd, rc == 0 ? 200 : 400,
                  rc == 0 ? "{\"ok\":true}" : "{\"error\":\"resume failed\"}");
        close(fd);
        return;
    }

    /* GET /api/transfers — status of all transfers */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/transfers") == 0) {
        Transfer ts[MAX_TRANSFERS];
        int n = transfer_get_all(ts, MAX_TRANSFERS);

        const char *state_names[] = { "idle", "active", "paused", "done", "error" };
        std::string json = "[";
        for (int i = 0; i < n; i++) {
            if (i) json += ",";
            int pct = ts[i].total_chunks > 0
                ? (int)(ts[i].done_chunks * 100 / ts[i].total_chunks) : 0;
            json += "{\"id\":" + std::to_string(ts[i].id)
                 + ",\"filename\":\"" + json_escape(ts[i].filename) + "\""
                 + ",\"peer\":\"" + json_escape(ts[i].peer) + "\""
                 + ",\"state\":\"" + state_names[ts[i].state] + "\""
                 + ",\"done\":" + std::to_string(ts[i].done_chunks)
                 + ",\"total\":" + std::to_string(ts[i].total_chunks)
                 + ",\"percent\":" + std::to_string(pct) + "}";
        }
        json += "]";
        send_json(fd, 200, json);
        close(fd);
        return;
    }

    /* OPTIONS for CORS preflight */
    if (strcmp(req->method, "OPTIONS") == 0) {
        const char *hdr =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n";
        send(fd, hdr, strlen(hdr), 0);
        close(fd);
        return;
    }

    send_response(fd, 404, "text/plain", "Not Found", 9);
    close(fd);
}

/* ── SSE event pump: drains client event queue ──────────── */

static pthread_t pump_thread;
static volatile int pump_running = 0;

static void *event_pump(void *arg)
{
    (void)arg;
    while (pump_running) {
        ChatEvent ev;
        while (client_poll_event(&ev)) {
            if (ev.type == EVT_CHAT) {
                char json[MAX_MSG + 256];
                snprintf(json, sizeof(json),
                         "{\"from\":\"%s\",\"text\":\"%s\",\"ts\":%ld}",
                         ev.from, ev.text, ev.timestamp);
                sse_broadcast("chat", json);
            } else {
                const char *state_name = "active";
                const char *event_name = "file_progress";
                if (ev.xfer_state == XFER_DONE)   { state_name = "done";  event_name = "file_complete"; }
                if (ev.xfer_state == XFER_ERROR)  { state_name = "error"; event_name = "file_error"; }
                if (ev.xfer_state == XFER_PAUSED) { state_name = "paused"; }

                int pct = ev.total_chunks > 0
                    ? (int)(ev.done_chunks * 100 / ev.total_chunks) : 0;

                char json[1024];
                snprintf(json, sizeof(json),
                         "{\"id\":%d,\"filename\":\"%s\",\"peer\":\"%s\","
                         "\"state\":\"%s\",\"done\":%u,\"total\":%u,\"percent\":%d}",
                         ev.xfer_id, ev.filename, ev.from,
                         state_name, ev.done_chunks, ev.total_chunks, pct);
                sse_broadcast(event_name, json);
            }
        }
        usleep(50000); /* 50ms */
    }
    return NULL;
}

/* ── Main HTTP loop ─────────────────────────────────────── */

static void *http_loop(void *arg)
{
    int port = *(int *)arg;

    http_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (http_fd < 0) { util_log(LOG_ERROR, "http socket: %s", strerror(errno)); return NULL; }

    int yes = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(http_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        util_log(LOG_ERROR, "http bind: %s", strerror(errno));
        close(http_fd); http_fd = -1;
        return NULL;
    }

    listen(http_fd, 16);
    util_log(LOG_INFO, "http: serving on http://localhost:%d", port);

    pump_running = 1;
    pthread_create(&pump_thread, NULL, event_pump, NULL);

    while (http_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(http_fd, &rfds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(http_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(http_fd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) continue;

        HttpReq req;
        if (parse_request(cfd, &req) == 0) {
            handle_request(cfd, &req);
        } else {
            close(cfd);
        }
    }

    pump_running = 0;
    pthread_join(pump_thread, NULL);

    pthread_mutex_lock(&sse_lock);
    for (int i = 0; i < sse_count; i++)
        close(sse_fds[i]);
    sse_count = 0;
    pthread_mutex_unlock(&sse_lock);

    close(http_fd);
    http_fd = -1;
    return NULL;
}

static int saved_port;

void http_start(int port)
{
    if (http_running) return;
    http_running = 1;
    saved_port = port;
    pthread_create(&http_thread, NULL, http_loop, &saved_port);
}

void http_stop(void)
{
    if (!http_running) return;
    http_running = 0;
    pthread_join(http_thread, NULL);
}
