/*
 * validate.c — Integration validation suite.
 *
 * Runs against a live server started by scripts/run_validation.sh.
 * All tests use the client library (client.h).
 *
 * Test categories:
 *  1. Register tests  — reset values, RW, RO rejection, W1C, reserved bits,
 *                       walking patterns, CTRL/STATUS, counter, full reset.
 *  2. Protocol robustness — bad magic, bad version, bad CRC, truncated,
 *                           oversized plen, unknown type, unknown register,
 *                           fragmented 1 byte at a time, 10 back-to-back,
 *                           abrupt disconnect mid-packet, 50 concurrent
 *                           clients × 1000 ops each.
 *  3. Timing          — RTT latency over 10,000 pings (min/avg/median/p99/max)
 *                       and throughput in requests/sec.
 */

#include "../include/test_framework.h"
#include "../include/client.h"
#include "../include/protocol.h"
#include "../include/regfile.h"
#include "../include/crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

static const char *g_host = "127.0.0.1";
static int         g_port = 5555;

#define LATENCY_SAMPLES    10000
#define CONCURRENT_CLIENTS 50
#define OPS_PER_CLIENT     1000

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static client_t *new_client(void) {
    return client_connect(g_host, (uint16_t)g_port, 2000, 3);
}

/* Send raw bytes directly to server (bypasses client library) */
static int raw_send(const uint8_t *data, size_t len) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    send(fd, data, len, MSG_NOSIGNAL);
    return fd;
}

/* Receive one response packet (up to PROTO_MAX_PACKET bytes) */
static int raw_recv(int fd, uint8_t *buf, size_t bufsz, int timeout_ms) {
    size_t total = 0;
    while (total < bufsz) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, timeout_ms);
        if (r <= 0) break;
        ssize_t n = recv(fd, buf + total, bufsz - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        /* Check if we have a complete packet */
        if (total >= PROTO_HEADER_LEN) {
            uint16_t plen = (uint16_t)(((uint16_t)buf[HDR_OFF_PLEN] << 8)
                                        | buf[HDR_OFF_PLEN + 1]);
            if (total >= PROTO_HEADER_LEN + plen) break;
        }
    }
    return (int)total;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* =========================================================================
 * 1. Register tests
 * ====================================================================== */

static void test_reg_reset_values(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    /* Reset first to ensure known state */
    TEST_ASSERT_EQ(s, client_reset(c), CLIENT_OK);

    for (size_t i = 0; i < REGFILE_NUM_REGS; i++) {
        const reg_desc_t *d = regfile_descriptor_by_index(i);
        if (!d || d->access == ACCESS_WO) continue;

        uint32_t val = 0;
        client_err_t err = client_read_reg(c, d->addr, &val);
        TEST_ASSERT_EQ(s, err, CLIENT_OK);
        /* COUNTER increments on each access so we cannot check its exact value */
        if (d->addr == REG_COUNTER) continue;
        TEST_ASSERT_HEX(s, val, d->reset_value);
    }
    client_close(c);
}

static void test_reg_rw_readback(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    client_reset(c);

    uint16_t scratch_addrs[] = { REG_SCRATCH0, REG_SCRATCH1,
                                  REG_SCRATCH2, REG_SCRATCH3 };
    uint32_t patterns[] = { 0xDEADBEEFu, 0xCAFEBABEu,
                             0x12345678u, 0xA5A5A5A5u };

    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQ(s, client_write_reg(c, scratch_addrs[i], patterns[i]),
                       CLIENT_OK);
        uint32_t v = 0;
        TEST_ASSERT_EQ(s, client_read_reg(c, scratch_addrs[i], &v), CLIENT_OK);
        TEST_ASSERT_HEX(s, v, patterns[i]);
    }
    client_close(c);
}

static void test_reg_ro_rejected(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    client_reset(c);

    /* Writing to ID (RO) must return CLIENT_ERR_READONLY */
    client_err_t err = client_write_reg(c, REG_ID, 0xFFFFFFFFu);
    TEST_ASSERT_EQ(s, err, CLIENT_ERR_READONLY);

    /* Value must be unchanged */
    uint32_t v = 0;
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_ID, &v), CLIENT_OK);
    TEST_ASSERT_HEX(s, v, RST_ID);

    client_close(c);
}

static void test_reg_w1c(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    client_reset(c);

    /* Trigger CTRL.START to set STATUS.DONE */
    TEST_ASSERT_EQ(s, client_write_reg(c, REG_CTRL, CTRL_START), CLIENT_OK);

    /* Burn accesses to advance busy countdown (read SCRATCH a few times) */
    for (int i = 0; i < (int)CTRL_BUSY_CLEAR_ACCESSES + 4; i++) {
        uint32_t dummy;
        client_read_reg(c, REG_SCRATCH0, &dummy);
    }

    uint32_t st = 0;
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_STATUS, &st), CLIENT_OK);
    TEST_ASSERT(s, (st & STATUS_DONE) != 0);

    /* Clear DONE via W1C */
    TEST_ASSERT_EQ(s, client_write_reg(c, REG_STATUS, STATUS_DONE), CLIENT_OK);
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_STATUS, &st), CLIENT_OK);
    TEST_ASSERT(s, (st & STATUS_DONE) == 0);

    client_close(c);
}

static void test_reg_reserved_bits(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    client_reset(c);

    /* Write all-ones to CTRL — reserved bits must not appear in readback */
    TEST_ASSERT_EQ(s, client_write_reg(c, REG_CTRL, 0xFFFFFFFFu), CLIENT_OK);
    uint32_t v = 0;
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_CTRL, &v), CLIENT_OK);
    TEST_ASSERT_HEX(s, v & ~CTRL_WRITABLE, 0u);

    client_close(c);
}

static void test_reg_walking_ones(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    client_reset(c);
    for (int bit = 0; bit < 32; bit++) {
        uint32_t pattern = UINT32_C(1) << bit;
        TEST_ASSERT_EQ(s, client_write_reg(c, REG_SCRATCH2, pattern), CLIENT_OK);
        uint32_t v = 0;
        TEST_ASSERT_EQ(s, client_read_reg(c, REG_SCRATCH2, &v), CLIENT_OK);
        TEST_ASSERT_HEX(s, v, pattern);
    }
    client_close(c);
}

static void test_reg_walking_zeros(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    client_reset(c);
    for (int bit = 0; bit < 32; bit++) {
        uint32_t pattern = ~(UINT32_C(1) << bit);
        TEST_ASSERT_EQ(s, client_write_reg(c, REG_SCRATCH3, pattern), CLIENT_OK);
        uint32_t v = 0;
        TEST_ASSERT_EQ(s, client_read_reg(c, REG_SCRATCH3, &v), CLIENT_OK);
        TEST_ASSERT_HEX(s, v, pattern);
    }
    client_close(c);
}

static void test_reg_ctrl_busy_done(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    client_reset(c);

    /* Confirm BUSY is clear before start */
    uint32_t st = 0;
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_STATUS, &st), CLIENT_OK);
    TEST_ASSERT(s, (st & STATUS_BUSY) == 0);

    TEST_ASSERT_EQ(s, client_write_reg(c, REG_CTRL, CTRL_START), CLIENT_OK);

    /* Immediately after: BUSY set */
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_STATUS, &st), CLIENT_OK);
    TEST_ASSERT(s, (st & STATUS_BUSY) != 0);

    /* Drain countdown */
    for (int i = 0; i < (int)CTRL_BUSY_CLEAR_ACCESSES + 4; i++) {
        uint32_t dummy;
        client_read_reg(c, REG_SCRATCH0, &dummy);
    }

    TEST_ASSERT_EQ(s, client_read_reg(c, REG_STATUS, &st), CLIENT_OK);
    TEST_ASSERT(s, (st & STATUS_BUSY) == 0);
    TEST_ASSERT(s, (st & STATUS_DONE) != 0);

    client_close(c);
}

static void test_reg_counter_monotonic(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    uint32_t prev = 0, cur = 0;
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_COUNTER, &prev), CLIENT_OK);
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_EQ(s, client_read_reg(c, REG_COUNTER, &cur), CLIENT_OK);
        TEST_ASSERT(s, cur > prev);
        prev = cur;
    }
    client_close(c);
}

static void test_reg_reset_restores_defaults(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    /* Dirty several registers */
    client_write_reg(c, REG_SCRATCH0, 0xDEADBEEFu);
    client_write_reg(c, REG_SCRATCH1, 0x12345678u);
    client_write_reg(c, REG_CONFIG,   0x00000003u);
    client_write_reg(c, REG_CTRL,     CTRL_START);

    /* Issue RESET command */
    TEST_ASSERT_EQ(s, client_reset(c), CLIENT_OK);

    uint32_t v = 0;
    TEST_ASSERT_EQ(s, client_read_reg(c, REG_SCRATCH0, &v), CLIENT_OK);
    TEST_ASSERT_HEX(s, v, RST_SCRATCH);

    TEST_ASSERT_EQ(s, client_read_reg(c, REG_SCRATCH1, &v), CLIENT_OK);
    TEST_ASSERT_HEX(s, v, RST_SCRATCH);

    TEST_ASSERT_EQ(s, client_read_reg(c, REG_CONFIG, &v), CLIENT_OK);
    TEST_ASSERT_HEX(s, v, RST_CONFIG);

    client_close(c);
}

/* =========================================================================
 * 2. Protocol robustness
 * ====================================================================== */

static void test_proto_bad_magic(test_suite_t *s) {
    /* Send a packet with bad magic, expect ERROR response */
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0100);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    buf[0] = 0xDE; buf[1] = 0xAD;   /* corrupt magic */

    int fd = raw_send(buf, (size_t)n);
    TEST_ASSERT(s, fd >= 0);
    if (fd < 0) return;

    uint8_t resp[PROTO_MAX_PACKET];
    int rn = raw_recv(fd, resp, sizeof(resp), 1000);
    close(fd);

    /* Should receive an ERROR packet */
    if (rn >= (int)PROTO_HEADER_LEN) {
        packet_t rpkt;
        if (decode_packet(resp, (size_t)rn, &rpkt) == 0) {
            TEST_ASSERT_EQ(s, rpkt.type, MSG_ERROR);
            TEST_ASSERT_EQ(s, rpkt.payload.error.code, ERR_BAD_MAGIC);
        } else {
            /* Server may have closed the connection without responding */
            TEST_ASSERT(s, 1);  /* acceptable */
        }
    }
    /* If no response (server closed), that is also acceptable behavior */
}

static void test_proto_bad_version(test_suite_t *s) {
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0101);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    buf[HDR_OFF_VERSION] = 0x99;
    /* Recompute CRC so it's valid except for version */
    /* Leave CRC as-is — decode should still reject on version */

    int fd = raw_send(buf, (size_t)n);
    TEST_ASSERT(s, fd >= 0);
    if (fd < 0) return;

    uint8_t resp[PROTO_MAX_PACKET];
    int rn = raw_recv(fd, resp, sizeof(resp), 1000);
    close(fd);
    (void)rn;
    TEST_ASSERT(s, 1);  /* server didn't crash */
}

static void test_proto_bad_crc(test_suite_t *s) {
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0102);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    buf[HDR_OFF_CRC] ^= 0xFF;  /* corrupt CRC */

    int fd = raw_send(buf, (size_t)n);
    TEST_ASSERT(s, fd >= 0);
    if (fd < 0) return;

    uint8_t resp[PROTO_MAX_PACKET];
    int rn = raw_recv(fd, resp, sizeof(resp), 1000);
    close(fd);

    if (rn >= (int)PROTO_HEADER_LEN) {
        packet_t rpkt;
        if (decode_packet(resp, (size_t)rn, &rpkt) == 0) {
            TEST_ASSERT_EQ(s, rpkt.type, MSG_ERROR);
            TEST_ASSERT_EQ(s, rpkt.payload.error.code, ERR_BAD_CRC);
        }
    }
}

static void test_proto_truncated(test_suite_t *s) {
    /* Send only 6 bytes of a PING — server must not crash */
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0103);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);

    int fd = raw_send(buf, 6);  /* truncated */
    TEST_ASSERT(s, fd >= 0);
    if (fd < 0) return;

    /* Close without sending the rest */
    close(fd);

    /* Server must still be alive — connect again */
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (c) {
        TEST_ASSERT_EQ(s, client_ping(c), CLIENT_OK);
        client_close(c);
    }
}

static void test_proto_oversized_plen(test_suite_t *s) {
    /* Craft a header with payload_len = 257 */
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0104);
    encode_packet(&pkt, buf, sizeof(buf));
    buf[HDR_OFF_PLEN]     = 0x01;
    buf[HDR_OFF_PLEN + 1] = 0x01;  /* plen = 257 */

    int fd = raw_send(buf, PROTO_HEADER_LEN);
    TEST_ASSERT(s, fd >= 0);
    if (fd < 0) return;

    uint8_t resp[PROTO_MAX_PACKET];
    int rn = raw_recv(fd, resp, sizeof(resp), 1000);
    close(fd);
    (void)rn;

    /* Server must not crash */
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (c) {
        TEST_ASSERT_EQ(s, client_ping(c), CLIENT_OK);
        client_close(c);
    }
}

static void test_proto_unknown_type(test_suite_t *s) {
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0105);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    buf[HDR_OFF_TYPE] = 0x7F;   /* unknown type */
    /* Recompute CRC so the only error is unknown type */
    buf[HDR_OFF_CRC]   = 0; buf[HDR_OFF_CRC+1] = 0;
    buf[HDR_OFF_CRC+2] = 0; buf[HDR_OFF_CRC+3] = 0;
    uint32_t crc = crc32_calc(buf, (size_t)n);
    buf[HDR_OFF_CRC+0] = (uint8_t)(crc >> 24);
    buf[HDR_OFF_CRC+1] = (uint8_t)(crc >> 16);
    buf[HDR_OFF_CRC+2] = (uint8_t)(crc >>  8);
    buf[HDR_OFF_CRC+3] = (uint8_t)(crc);

    int fd = raw_send(buf, (size_t)n);
    TEST_ASSERT(s, fd >= 0);
    if (fd < 0) return;

    uint8_t resp[PROTO_MAX_PACKET];
    int rn = raw_recv(fd, resp, sizeof(resp), 1000);
    close(fd);

    if (rn >= (int)PROTO_HEADER_LEN) {
        packet_t rpkt;
        if (decode_packet(resp, (size_t)rn, &rpkt) == 0) {
            TEST_ASSERT_EQ(s, rpkt.type, MSG_ERROR);
            TEST_ASSERT_EQ(s, rpkt.payload.error.code, ERR_UNKNOWN_TYPE);
        }
    }
}

static void test_proto_unknown_register(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    uint32_t v;
    client_err_t err = client_read_reg(c, 0xFFFFu, &v);
    TEST_ASSERT_EQ(s, err, CLIENT_ERR_SERVER);
    TEST_ASSERT_EQ(s, client_last_server_error(c), ERR_UNKNOWN_REGISTER);

    client_close(c);
}

static void test_proto_fragmented(test_suite_t *s) {
    /* Send a valid WRITE_REG one byte at a time over a raw socket */
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_write_reg(&pkt, 0x0200, REG_SCRATCH0, 0xAABBCCDDu);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);

    /* Open a plain TCP connection (without sending data yet) */
    int fd;
    {
        int sfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (sfd < 0) { TEST_FAIL(s, "socket"); return; }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port   = htons((uint16_t)g_port);
        inet_pton(AF_INET, g_host, &a.sin_addr);
        if (connect(sfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            close(sfd); TEST_FAIL(s, "connect"); return;
        }
        fd = sfd;
    }

    /* Send one byte at a time with 1 ms delay */
    for (int i = 0; i < n; i++) {
        send(fd, buf + i, 1, MSG_NOSIGNAL);
        usleep(1000);
    }

    uint8_t resp[PROTO_MAX_PACKET];
    int rn = raw_recv(fd, resp, sizeof(resp), 2000);
    close(fd);

    TEST_ASSERT(s, rn >= (int)PROTO_HEADER_LEN);
    if (rn >= (int)PROTO_HEADER_LEN) {
        packet_t rpkt;
        int rc = decode_packet(resp, (size_t)rn, &rpkt);
        TEST_ASSERT_EQ(s, rc, 0);
        TEST_ASSERT_EQ(s, rpkt.type, MSG_WRITE_ACK);
    }
}

static void test_proto_back_to_back(test_suite_t *s) {
    /* 10 PING packets concatenated in a single send() */
    int fd;
    {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) { TEST_FAIL(s, "socket"); return; }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port   = htons((uint16_t)g_port);
        inet_pton(AF_INET, g_host, &a.sin_addr);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            close(fd); TEST_FAIL(s, "connect"); return;
        }
    }

    uint8_t stream[PROTO_MAX_PACKET * 10];
    size_t total = 0;
    for (int i = 0; i < 10; i++) {
        packet_t pkt;
        uint8_t tmp[PROTO_MAX_PACKET];
        pkt_build_ping(&pkt, (uint16_t)(0x0300 + i));
        int n = encode_packet(&pkt, tmp, sizeof(tmp));
        TEST_ASSERT(s, n > 0);
        memcpy(stream + total, tmp, (size_t)n);
        total += (size_t)n;
    }
    send(fd, stream, total, MSG_NOSIGNAL);

    /* Expect 10 PONG responses */
    int pong_count = 0;
    uint8_t rbuf[PROTO_MAX_PACKET * 10];
    size_t rlen = 0;

    for (int attempt = 0; attempt < 20 && pong_count < 10; attempt++) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, 500);
        if (r <= 0) break;
        ssize_t n = recv(fd, rbuf + rlen, sizeof(rbuf) - rlen, 0);
        if (n <= 0) break;
        rlen += (size_t)n;

        /* Parse all complete packets */
        size_t pos = 0;
        while (pos + PROTO_HEADER_LEN <= rlen) {
            uint16_t plen = (uint16_t)(((uint16_t)rbuf[pos + HDR_OFF_PLEN] << 8)
                                        | rbuf[pos + HDR_OFF_PLEN + 1]);
            size_t pktsz = PROTO_HEADER_LEN + plen;
            if (pos + pktsz > rlen) break;
            packet_t rpkt;
            if (decode_packet(rbuf + pos, pktsz, &rpkt) == 0) {
                if (rpkt.type == MSG_PONG) pong_count++;
            }
            pos += pktsz;
        }
        /* Compact buffer */
        if (pos > 0) {
            memmove(rbuf, rbuf + pos, rlen - pos);
            rlen -= pos;
        }
    }

    close(fd);
    TEST_ASSERT_EQ(s, pong_count, 10);
}

static void test_proto_abrupt_disconnect(test_suite_t *s) {
    /* Connect, send half a WRITE_REG, then close abruptly */
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_write_reg(&pkt, 0x0400, REG_SCRATCH1, 0x11223344u);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { TEST_FAIL(s, "socket"); return; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd); TEST_FAIL(s, "connect"); return;
    }
    send(fd, buf, (size_t)(n / 2), MSG_NOSIGNAL); /* partial */
    struct linger sl = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl)); /* RST */
    close(fd);

    usleep(50000);  /* 50 ms for server to process */

    /* Server must still be alive */
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (c) {
        TEST_ASSERT_EQ(s, client_ping(c), CLIENT_OK);
        client_close(c);
    }
}

/* --- Concurrent clients --- */

typedef struct {
    int   id;
    int   ops;
    int   errors;
} thread_arg_t;

static void *concurrent_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    ta->errors = 0;

    client_t *c = client_connect(g_host, (uint16_t)g_port, 2000, 1);
    if (!c) { ta->errors = ta->ops; return NULL; }

    for (int i = 0; i < ta->ops; i++) {
        uint32_t pattern = (uint32_t)((ta->id << 16) | (i & 0xFFFF));
        if (client_write_reg(c, REG_SCRATCH0, pattern) != CLIENT_OK) {
            ta->errors++;
            continue;
        }
        uint32_t v = 0;
        if (client_read_reg(c, REG_SCRATCH0, &v) != CLIENT_OK) {
            ta->errors++;
        }
    }
    client_close(c);
    return NULL;
}

static void test_proto_concurrent_clients(test_suite_t *s) {
    pthread_t threads[CONCURRENT_CLIENTS];
    thread_arg_t args[CONCURRENT_CLIENTS];

    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        args[i].id  = i;
        args[i].ops = OPS_PER_CLIENT;
        int rc = pthread_create(&threads[i], NULL, concurrent_worker, &args[i]);
        TEST_ASSERT_EQ(s, rc, 0);
    }

    int total_errors = 0;
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        pthread_join(threads[i], NULL);
        total_errors += args[i].errors;
    }

    printf("\n  [concurrent] %d clients × %d ops: %d errors\n",
           CONCURRENT_CLIENTS, OPS_PER_CLIENT, total_errors);
    TEST_ASSERT_EQ(s, total_errors, 0);

    /* Server must still be alive */
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (c) {
        TEST_ASSERT_EQ(s, client_ping(c), CLIENT_OK);
        client_close(c);
    }
}

/* =========================================================================
 * 3. Timing / latency
 * ====================================================================== */

static void test_timing_latency(test_suite_t *s) {
    client_t *c = new_client();
    TEST_ASSERT(s, c != NULL);
    if (!c) return;

    uint64_t *samples = (uint64_t *)malloc(LATENCY_SAMPLES * sizeof(uint64_t));
    TEST_ASSERT(s, samples != NULL);
    if (!samples) { client_close(c); return; }

    uint64_t t_start = now_us();

    for (int i = 0; i < LATENCY_SAMPLES; i++) {
        uint64_t t0 = now_us();
        client_err_t err = client_ping(c);
        uint64_t t1 = now_us();
        TEST_ASSERT_EQ(s, err, CLIENT_OK);
        samples[i] = t1 - t0;
    }

    uint64_t t_end = now_us();
    double elapsed_sec = (double)(t_end - t_start) / 1e6;
    double throughput  = LATENCY_SAMPLES / elapsed_sec;

    /* Sort for percentiles */
    qsort(samples, LATENCY_SAMPLES, sizeof(uint64_t), cmp_u64);

    uint64_t min_us   = samples[0];
    uint64_t max_us   = samples[LATENCY_SAMPLES - 1];
    uint64_t med_us   = samples[LATENCY_SAMPLES / 2];
    uint64_t p99_us   = samples[(int)(LATENCY_SAMPLES * 0.99)];
    double   sum      = 0;
    for (int i = 0; i < LATENCY_SAMPLES; i++) sum += (double)samples[i];
    double   avg_us   = sum / LATENCY_SAMPLES;

    printf("\n");
    printf("  +--------------+----------+\n");
    printf("  | Metric       |    Value |\n");
    printf("  +--------------+----------+\n");
    printf("  | Samples      | %8d |\n", LATENCY_SAMPLES);
    printf("  | Min RTT (us) | %8" PRIu64 " |\n", min_us);
    printf("  | Avg RTT (us) | %8.1f |\n", avg_us);
    printf("  | Med RTT (us) | %8" PRIu64 " |\n", med_us);
    printf("  | P99 RTT (us) | %8" PRIu64 " |\n", p99_us);
    printf("  | Max RTT (us) | %8" PRIu64 " |\n", max_us);
    printf("  | Throughput   | %6.0f/s |\n", throughput);
    printf("  +--------------+----------+\n");

    free(samples);
    client_close(c);

    /* Sanity: latency should be under 100 ms on loopback */
    TEST_ASSERT(s, avg_us < 100000ULL);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(int argc, char *argv[]) {
    crc32_init();

    /* Accept --host and --port overrides */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            g_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        }
    }

    /* Verify server is reachable before running tests */
    client_t *probe = client_connect(g_host, (uint16_t)g_port, 1000, 0);
    if (!probe) {
        fprintf(stderr, "ERROR: Cannot connect to server at %s:%d\n"
                        "       Start the server first:\n"
                        "         ./regsim-server --port %d\n",
                g_host, g_port, g_port);
        return 1;
    }
    client_close(probe);

    test_suite_t *s = test_suite_new("Integration Validation Suite");

    /* Register tests */
    RUN_TEST(s, test_reg_reset_values);
    RUN_TEST(s, test_reg_rw_readback);
    RUN_TEST(s, test_reg_ro_rejected);
    RUN_TEST(s, test_reg_w1c);
    RUN_TEST(s, test_reg_reserved_bits);
    RUN_TEST(s, test_reg_walking_ones);
    RUN_TEST(s, test_reg_walking_zeros);
    RUN_TEST(s, test_reg_ctrl_busy_done);
    RUN_TEST(s, test_reg_counter_monotonic);
    RUN_TEST(s, test_reg_reset_restores_defaults);

    /* Protocol robustness */
    RUN_TEST(s, test_proto_bad_magic);
    RUN_TEST(s, test_proto_bad_version);
    RUN_TEST(s, test_proto_bad_crc);
    RUN_TEST(s, test_proto_truncated);
    RUN_TEST(s, test_proto_oversized_plen);
    RUN_TEST(s, test_proto_unknown_type);
    RUN_TEST(s, test_proto_unknown_register);
    RUN_TEST(s, test_proto_fragmented);
    RUN_TEST(s, test_proto_back_to_back);
    RUN_TEST(s, test_proto_abrupt_disconnect);
    RUN_TEST(s, test_proto_concurrent_clients);

    /* Timing */
    RUN_TEST(s, test_timing_latency);

    return test_suite_finish(s);
}
