/*
 * client.c — Blocking TCP client library for the register simulator.
 *
 * Each operation:
 *   1. Encodes the request packet.
 *   2. Sends it in full (handles partial writes via loop).
 *   3. Waits for a response using poll() with a configurable timeout.
 *   4. On timeout: retries up to `retries` times with the SAME sequence
 *      number.
 *   5. Decodes the response, validates the sequence number, and returns.
 */

#include "client.h"
#include "protocol.h"
#include "crc32.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* -------------------------------------------------------------------------
 * Client handle (concrete)
 * ---------------------------------------------------------------------- */

struct client {
    int      fd;
    int      timeout_ms;
    int      retries;
    uint16_t next_seq;
    uint8_t  last_srv_err;

    /* Receive accumulation buffer for streaming reads */
    uint8_t  recv_buf[PROTO_MAX_PACKET * 4];
    size_t   recv_len;
};

/* -------------------------------------------------------------------------
 * Internal: send all bytes (handles partial writes)
 * ---------------------------------------------------------------------- */

static client_err_t send_all(int fd, const uint8_t *buf, size_t len,
                              int timeout_ms) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd;
        pfd.fd     = fd;
        pfd.events = POLLOUT;
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0)  return CLIENT_ERR_SEND;
        if (r == 0) return CLIENT_ERR_TIMEOUT;
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return CLIENT_ERR_SEND;
        }
        sent += (size_t)n;
    }
    return CLIENT_OK;
}

/* -------------------------------------------------------------------------
 * Internal: receive until we have a complete, CRC-valid packet.
 * Uses poll() with deadline.  Handles partial reads.
 * ---------------------------------------------------------------------- */

static client_err_t recv_response(struct client *c, packet_t *resp) {
    /* We accumulate bytes in c->recv_buf.  After each chunk we try to
     * decode.  If decode_packet returns -1 (truncated) we wait for more.
     * Any other error is a protocol violation. */

    for (;;) {
        /* Try to decode from what we already have */
        if (c->recv_len >= PROTO_HEADER_LEN) {
            /* Peek at payload_len from the buffer */
            uint16_t plen = (uint16_t)(((uint16_t)c->recv_buf[HDR_OFF_PLEN] << 8)
                                        | c->recv_buf[HDR_OFF_PLEN + 1]);
            size_t total = PROTO_HEADER_LEN + plen;

            if (c->recv_len >= total) {
                int rc = decode_packet(c->recv_buf, total, resp);
                if (rc == 0) {
                    /* Consume these bytes */
                    size_t remaining = c->recv_len - total;
                    if (remaining > 0) {
                        memmove(c->recv_buf, c->recv_buf + total, remaining);
                    }
                    c->recv_len = remaining;
                    return CLIENT_OK;
                }
                if (rc == -2) return CLIENT_ERR_PROTOCOL; /* bad magic */
                if (rc == -3) return CLIENT_ERR_PROTOCOL; /* bad version */
                if (rc == -4) return CLIENT_ERR_PROTOCOL; /* bad CRC */
                if (rc == -5) return CLIENT_ERR_PROTOCOL; /* bad length */
                /* rc == -1 should not happen here since recv_len >= total */
                return CLIENT_ERR_PROTOCOL;
            }
        }

        /* Not enough bytes — receive more */
        size_t space = sizeof(c->recv_buf) - c->recv_len;
        if (space == 0) return CLIENT_ERR_PROTOCOL; /* buffer overflow */

        struct pollfd pfd;
        pfd.fd     = c->fd;
        pfd.events = POLLIN;
        int r = poll(&pfd, 1, c->timeout_ms);
        if (r < 0)  return CLIENT_ERR_RECV;
        if (r == 0) return CLIENT_ERR_TIMEOUT;

        ssize_t n = recv(c->fd, c->recv_buf + c->recv_len, space, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return CLIENT_ERR_RECV;
        }
        if (n == 0) return CLIENT_ERR_RECV; /* connection closed */
        c->recv_len += (size_t)n;
    }
}

/* -------------------------------------------------------------------------
 * Internal: send a packet and receive one response, with retries.
 * ---------------------------------------------------------------------- */

static client_err_t transact(struct client *c,
                              packet_t *req, packet_t *resp) {
    uint8_t buf[PROTO_MAX_PACKET];
    int n = encode_packet(req, buf, sizeof(buf));
    if (n < 0) return CLIENT_ERR_ARGS;

    for (int attempt = 0; attempt <= c->retries; attempt++) {
        client_err_t err = send_all(c->fd, buf, (size_t)n, c->timeout_ms);
        if (err != CLIENT_OK) {
            if (err == CLIENT_ERR_TIMEOUT && attempt < c->retries) continue;
            return err;
        }

        err = recv_response(c, resp);
        if (err == CLIENT_ERR_TIMEOUT && attempt < c->retries) {
            /* Resend with the same sequence number */
            continue;
        }
        if (err != CLIENT_OK) return err;

        /* Check sequence number */
        if (resp->sequence != req->sequence) {
            /* Could be a stale response from a previous timed-out attempt;
             * drain and retry once more. */
            if (attempt < c->retries) continue;
            return CLIENT_ERR_SEQ;
        }

        /* Check for server error */
        if (resp->type == MSG_ERROR) {
            c->last_srv_err = resp->payload.error.code;
            if (resp->payload.error.code == ERR_READ_ONLY_VIOLATION)
                return CLIENT_ERR_READONLY;
            return CLIENT_ERR_SERVER;
        }

        return CLIENT_OK;
    }
    return CLIENT_ERR_TIMEOUT;
}

/* -------------------------------------------------------------------------
 * client_connect
 * ---------------------------------------------------------------------- */

client_t *client_connect(const char *host, uint16_t port,
                          int timeout_ms, int retries) {
    crc32_init();

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) return NULL;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family,
                    rp->ai_socktype | SOCK_CLOEXEC,
                    rp->ai_protocol);
        if (fd < 0) continue;

        /* Set non-blocking for connect with timeout */
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd); fd = -1; continue;
        }

        int r = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (r == 0) {
            /* Connected immediately */
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd     = fd;
            pfd.events = POLLOUT;
            int pr = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 5000);
            if (pr <= 0) { close(fd); fd = -1; continue; }
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err) { close(fd); fd = -1; continue; }
        } else {
            close(fd); fd = -1; continue;
        }

        /* Restore blocking mode */
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        break;
    }
    freeaddrinfo(res);

    if (fd < 0) return NULL;

    /* TCP_NODELAY */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    client_t *c = (client_t *)calloc(1, sizeof(client_t));
    if (!c) { close(fd); return NULL; }
    c->fd          = fd;
    c->timeout_ms  = (timeout_ms > 0) ? timeout_ms : CLIENT_DEFAULT_TIMEOUT_MS;
    c->retries     = (retries >= 0)   ? retries    : CLIENT_DEFAULT_RETRIES;
    c->next_seq    = 1;
    c->last_srv_err= 0;
    return c;
}

/* -------------------------------------------------------------------------
 * client_close
 * ---------------------------------------------------------------------- */

void client_close(client_t *c) {
    if (!c) return;
    close(c->fd);
    free(c);
}

/* -------------------------------------------------------------------------
 * client_read_reg
 * ---------------------------------------------------------------------- */

client_err_t client_read_reg(client_t *c, uint16_t addr, uint32_t *value) {
    if (!c || !value) return CLIENT_ERR_ARGS;

    packet_t req, resp;
    pkt_build_read_reg(&req, c->next_seq++, addr);

    client_err_t err = transact(c, &req, &resp);
    if (err != CLIENT_OK) return err;

    if (resp.type != MSG_READ_RESP) return CLIENT_ERR_PROTOCOL;
    *value = resp.payload.read_resp.value;
    return CLIENT_OK;
}

/* -------------------------------------------------------------------------
 * client_write_reg
 * ---------------------------------------------------------------------- */

client_err_t client_write_reg(client_t *c, uint16_t addr, uint32_t value) {
    if (!c) return CLIENT_ERR_ARGS;

    packet_t req, resp;
    pkt_build_write_reg(&req, c->next_seq++, addr, value);

    client_err_t err = transact(c, &req, &resp);
    if (err != CLIENT_OK) return err;

    if (resp.type != MSG_WRITE_ACK) return CLIENT_ERR_PROTOCOL;
    return CLIENT_OK;
}

/* -------------------------------------------------------------------------
 * client_ping
 * ---------------------------------------------------------------------- */

client_err_t client_ping(client_t *c) {
    if (!c) return CLIENT_ERR_ARGS;

    packet_t req, resp;
    pkt_build_ping(&req, c->next_seq++);

    client_err_t err = transact(c, &req, &resp);
    if (err != CLIENT_OK) return err;

    if (resp.type != MSG_PONG) return CLIENT_ERR_PROTOCOL;
    return CLIENT_OK;
}

/* -------------------------------------------------------------------------
 * client_reset
 * ---------------------------------------------------------------------- */

client_err_t client_reset(client_t *c) {
    if (!c) return CLIENT_ERR_ARGS;

    packet_t req, resp;
    pkt_build_reset(&req, c->next_seq++);

    client_err_t err = transact(c, &req, &resp);
    if (err != CLIENT_OK) return err;

    if (resp.type != MSG_RESET_ACK) return CLIENT_ERR_PROTOCOL;
    return CLIENT_OK;
}

/* -------------------------------------------------------------------------
 * client_strerror
 * ---------------------------------------------------------------------- */

const char *client_strerror(client_err_t err) {
    switch (err) {
    case CLIENT_OK:           return "success";
    case CLIENT_ERR_CONNECT:  return "connection failed";
    case CLIENT_ERR_SEND:     return "send error";
    case CLIENT_ERR_RECV:     return "receive error";
    case CLIENT_ERR_TIMEOUT:  return "operation timed out";
    case CLIENT_ERR_PROTOCOL: return "protocol error (bad magic/version/CRC)";
    case CLIENT_ERR_SEQ:      return "sequence number mismatch";
    case CLIENT_ERR_SERVER:   return "server returned an error";
    case CLIENT_ERR_ARGS:     return "invalid arguments";
    case CLIENT_ERR_READONLY: return "register is read-only";
    default:                  return "unknown error";
    }
}

/* -------------------------------------------------------------------------
 * client_last_server_error
 * ---------------------------------------------------------------------- */

uint8_t client_last_server_error(const client_t *c) {
    if (!c) return 0;
    return c->last_srv_err;
}
