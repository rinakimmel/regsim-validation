/*
 * server_main.c — Entry point for the regsim server binary.
 *
 * Usage:
 *   regsim-server [--port PORT] [--idle-timeout SEC] [--log-level LEVEL]
 *                 [--udp]
 *
 * Defaults:
 *   --port          5555
 *   --idle-timeout  30
 *   --log-level     info
 */

#include "server.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --port PORT          TCP/UDP port to listen on (default: %d)\n"
        "  --idle-timeout SEC   Idle connection timeout in seconds (default: %d)\n"
        "  --log-level LEVEL    Logging level: error|warn|info|debug (default: info)\n"
        "  --udp                Use UDP instead of TCP\n"
        "  --help               Show this message\n",
        argv0,
        SERVER_DEFAULT_PORT,
        SERVER_DEFAULT_IDLE_TIMEOUT);
}

int main(int argc, char *argv[]) {
    server_config_t cfg = {
        .port            = SERVER_DEFAULT_PORT,
        .idle_timeout_sec= SERVER_DEFAULT_IDLE_TIMEOUT,
        .udp_mode        = 0,
        .log_level       = LOG_INFO,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --port requires an argument\n");
                return 1;
            }
            i++;
            char *end;
            long p = strtol(argv[i], &end, 10);
            if (*end || p < 1 || p > 65535) {
                fprintf(stderr, "error: invalid port: %s\n", argv[i]);
                return 1;
            }
            cfg.port = (uint16_t)p;
        } else if (strcmp(argv[i], "--idle-timeout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --idle-timeout requires an argument\n");
                return 1;
            }
            i++;
            char *end;
            long t = strtol(argv[i], &end, 10);
            if (*end || t < 0) {
                fprintf(stderr, "error: invalid idle-timeout: %s\n", argv[i]);
                return 1;
            }
            cfg.idle_timeout_sec = (int)t;
        } else if (strcmp(argv[i], "--log-level") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --log-level requires an argument\n");
                return 1;
            }
            i++;
            int lvl = log_level_from_string(argv[i]);
            if (lvl < 0) {
                fprintf(stderr, "error: unknown log level: %s\n", argv[i]);
                return 1;
            }
            cfg.log_level = lvl;
        } else if (strcmp(argv[i], "--udp") == 0) {
            cfg.udp_mode = 1;
        } else {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    return server_run(&cfg) == 0 ? 0 : 1;
}
