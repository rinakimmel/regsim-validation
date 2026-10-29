/*
 * client.h — Blocking client library for the register simulator.
 *
 * All operations use poll() for timeouts.  On timeout the request is
 * retried up to `retries` times with the same sequence number before
 * returning an error.
 */

#ifndef REGSIM_CLIENT_H
#define REGSIM_CLIENT_H

#include <stdint.h>
#include "protocol.h"

/* -------------------------------------------------------------------------
 * Client configuration defaults
 * ---------------------------------------------------------------------- */

#define CLIENT_DEFAULT_TIMEOUT_MS   2000    /* per-attempt timeout */
#define CLIENT_DEFAULT_RETRIES      3       /* retry attempts */

/* -------------------------------------------------------------------------
 * Error codes returned by client functions
 * ---------------------------------------------------------------------- */

typedef enum {
    CLIENT_OK           =  0,
    CLIENT_ERR_CONNECT  = -1,   /* TCP connect failed */
    CLIENT_ERR_SEND     = -2,   /* send() failed */
    CLIENT_ERR_RECV     = -3,   /* recv() failed */
    CLIENT_ERR_TIMEOUT  = -4,   /* no response within deadline */
    CLIENT_ERR_PROTOCOL = -5,   /* bad magic/version/CRC in response */
    CLIENT_ERR_SEQ      = -6,   /* response sequence mismatch */
    CLIENT_ERR_SERVER   = -7,   /* server returned MSG_ERROR */
    CLIENT_ERR_ARGS     = -8,   /* invalid arguments */
    CLIENT_ERR_READONLY = -9,   /* remote READ_ONLY_VIOLATION */
} client_err_t;

/* -------------------------------------------------------------------------
 * Client handle (opaque)
 * ---------------------------------------------------------------------- */

typedef struct client client_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/*
 * client_connect — resolve `host`:`port` and establish a TCP connection.
 *
 * `timeout_ms` is applied to each individual send/receive attempt.
 * `retries` is the number of retry attempts on timeout.
 *
 * Returns a heap-allocated client_t on success, NULL on error.
 * Call client_close() to release resources.
 */
client_t *client_connect(const char *host, uint16_t port,
                         int timeout_ms, int retries);

/*
 * client_close — flush any pending data, close the socket, and free *c.
 * Safe to call with NULL.
 */
void client_close(client_t *c);

/* -------------------------------------------------------------------------
 * Operations
 * ---------------------------------------------------------------------- */

/*
 * client_read_reg — send READ_REG, receive READ_RESP, store value in *value.
 * Returns CLIENT_OK or a CLIENT_ERR_* code.
 */
client_err_t client_read_reg(client_t *c, uint16_t addr, uint32_t *value);

/*
 * client_write_reg — send WRITE_REG, receive WRITE_ACK.
 * Returns CLIENT_OK or a CLIENT_ERR_* code.
 */
client_err_t client_write_reg(client_t *c, uint16_t addr, uint32_t value);

/*
 * client_ping — send PING, receive PONG.
 * Returns CLIENT_OK or a CLIENT_ERR_* code.
 */
client_err_t client_ping(client_t *c);

/*
 * client_reset — send RESET, receive RESET_ACK.
 * Returns CLIENT_OK or a CLIENT_ERR_* code.
 */
client_err_t client_reset(client_t *c);

/*
 * client_strerror — return a human-readable description of a CLIENT_ERR_*.
 */
const char *client_strerror(client_err_t err);

/*
 * client_last_server_error — after CLIENT_ERR_SERVER, return the err_code_t
 * the server sent, or 0 if unavailable.
 */
uint8_t client_last_server_error(const client_t *c);

#endif /* REGSIM_CLIENT_H */
