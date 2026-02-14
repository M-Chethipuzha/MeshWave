/* main.cpp
 * Entry point: parse args, start modules, open browser.
 * Supports --server, --client IP, or interactive mode via dashboard.
 */

extern "C" {
#include "discovery.h"
#include "server.h"
#include "client.h"
#include "util.h"
}
#include "http.h"
#include "protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>

static volatile int quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

static void open_browser(int port)
{
    char cmd[256];
#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd), "open http://localhost:%d &", port);
#else
    snprintf(cmd, sizeof(cmd), "xdg-open http://localhost:%d &", port);
#endif
    int rc = system(cmd);
    (void)rc;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --server NAME     Start directly as server\n");
    printf("  --client IP       Start directly as client connecting to IP\n");
    printf("  --name NAME       Set username (client mode, default: User)\n");
    printf("  --port PORT       HTTP port (default: %d)\n", HTTP_PORT);
    printf("  --no-browser      Don't auto-open browser\n");
    printf("  -h, --help        Show this help\n");
}

int main(int argc, char *argv[])
{
    const char *mode_flag    = NULL;
    const char *server_name  = NULL;
    const char *client_ip    = NULL;
    const char *user_name    = "User";
    int         http_port    = HTTP_PORT;
    int         no_browser   = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            mode_flag = "server";
            server_name = argv[++i];
        } else if (strcmp(argv[i], "--client") == 0 && i + 1 < argc) {
            mode_flag = "client";
            client_ip = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            user_name = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            http_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-browser") == 0) {
            no_browser = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    util_log(LOG_INFO, "MeshWave starting...");

    http_start(http_port);

    if (mode_flag && strcmp(mode_flag, "server") == 0) {
        server_start(server_name);
    } else if (mode_flag && strcmp(mode_flag, "client") == 0) {
        discovery_start_scan();
        client_connect(client_ip, DATA_PORT, user_name);
    } else {
        discovery_start_scan();
    }

    if (!no_browser)
        open_browser(http_port);

    util_log(LOG_INFO, "Dashboard at http://localhost:%d  (Ctrl+C to quit)", http_port);

    while (!quit)
        sleep(1);

    util_log(LOG_INFO, "Shutting down...");

    client_disconnect();
    server_stop();
    discovery_stop_scan();
    discovery_stop_announce();
    http_stop();

    util_log(LOG_INFO, "Goodbye.");
    return 0;
}
