/*
 * server.h — TCP/UDP register simulator server.
 *
 * Single-threaded, epoll-based event loop supporting up to MAX_CLIENTS
 * simultaneous TCP connections (or UDP datagrams).
 */

#ifndef REGSIM_SERVER_H
#define REGSIM_SERVER_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Server configuration
 * ---------------------------------------------------------------------- */

#define SERVER_DEFAULT_PORT         5555
#define SERVER_DEFAULT_IDLE_TIMEOUT 30      /* seconds */
#define SERVER_MAX_CLIENTS          64
#define SERVER_BACKLOG               8

/* Per-connection send queue: maximum bytes we'll buffer before dropping */
#define SEND_QUEUE_CAP              (64u * 1024u)

/* -------------------------------------------------------------------------
 * Server configuration structure
 * ---------------------------------------------------------------------- */

typedef struct {
    uint16_t port;
    int      idle_timeout_sec;  /* 0 = no timeout */
    int      udp_mode;          /* 0 = TCP, 1 = UDP */
    int      log_level;         /* LOG_* from log.h */
} server_config_t;

/* -------------------------------------------------------------------------
 * Server lifecycle
 * ---------------------------------------------------------------------- */

/*
 * server_run — create the server socket, enter the epoll event loop, and
 * block until SIGINT or SIGTERM is received, then perform graceful shutdown.
 *
 * Returns 0 on clean exit, -1 on fatal error.
 */
int server_run(const server_config_t *cfg);

#endif /* REGSIM_SERVER_H */
