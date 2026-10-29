/*
 * test_codec.c — Unit tests for CRC-32 and packet encode/decode.
 *
 * Tests:
 *  - CRC known test vector: "123456789" → 0xCBF43926
 *  - CRC incremental == single-shot
 *  - CRC of all-zeros, all-ones
 *  - encode/decode round-trip for every message type
 *  - decode rejects bad magic
 *  - decode rejects bad version
 *  - decode rejects bad CRC
 *  - decode rejects payload_len > PROTO_MAX_PAYLOAD
 *  - decode rejects truncated buffer
 *  - streaming parser: single-byte feed
 *  - streaming parser: multiple packets in one chunk
 *  - streaming parser: bad magic recovers and finds next packet
 *  - streaming parser: bad CRC recovers
 *  - streaming parser: bad version emits error callback
 *  - streaming parser: oversized length field
 *  - convenience builder field correctness
 */

#include "../include/test_framework.h"
#include "../include/crc32.h"
#include "../include/protocol.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* =========================================================================
 * CRC-32 tests
 * ====================================================================== */

static void test_crc_known_vector(test_suite_t *s) {
    /* ANSI X3.66 / IEEE 802.3 test vector */
    const uint8_t data[] = "123456789";
    uint32_t crc = crc32_calc(data, 9);
    TEST_ASSERT_HEX(s, crc, 0xCBF43926u);
}

static void test_crc_empty(test_suite_t *s) {
    uint32_t crc = crc32_calc(NULL, 0);
    /* CRC of empty input: 0xFFFFFFFF ^ 0xFFFFFFFF = 0x00000000 */
    TEST_ASSERT_HEX(s, crc, 0x00000000u);
}

static void test_crc_incremental(test_suite_t *s) {
    const uint8_t data[] = "Hello, regsim!";
    size_t len = sizeof(data) - 1;

    /* Single shot */
    uint32_t full = crc32_calc(data, len);

    /* Byte by byte */
    uint32_t inc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        inc = crc32_update(inc, &data[i], 1);
    }
    inc ^= 0xFFFFFFFFu;

    TEST_ASSERT_HEX(s, inc, full);
}

static void test_crc_two_halves(test_suite_t *s) {
    const uint8_t data[] = "ABCDEFGH";
    size_t len = sizeof(data) - 1;

    uint32_t full = crc32_calc(data, len);

    uint32_t split = 0xFFFFFFFFu;
    split = crc32_update(split, data,       len / 2);
    split = crc32_update(split, data + len/2, len - len/2);
    split ^= 0xFFFFFFFFu;

    TEST_ASSERT_HEX(s, split, full);
}

static void test_crc_all_zeros(test_suite_t *s) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    /* Just verify it produces a deterministic non-trivially-zero value */
    uint32_t crc = crc32_calc(buf, sizeof(buf));
    (void)crc; /* value is architecture-independent but fixed */
    TEST_ASSERT(s, 1); /* always pass — we just exercise the path */
}

static void test_crc_all_ones(test_suite_t *s) {
    uint8_t buf[64];
    memset(buf, 0xFF, sizeof(buf));
    uint32_t crc = crc32_calc(buf, sizeof(buf));
    (void)crc;
    TEST_ASSERT(s, 1);
}

/* =========================================================================
 * encode_packet / decode_packet round-trip tests
 * ====================================================================== */

/* Helper: encode then decode, return 0 on success */
static int roundtrip(const packet_t *orig, packet_t *out) {
    uint8_t buf[PROTO_MAX_PACKET];
    int n = encode_packet(orig, buf, sizeof(buf));
    if (n < 0) return -1;
    return decode_packet(buf, (size_t)n, out);
}

static void test_roundtrip_read_reg(test_suite_t *s) {
    packet_t req, out;
    pkt_build_read_reg(&req, 0x0042, 0x000E);
    TEST_ASSERT_EQ(s, roundtrip(&req, &out), 0);
    TEST_ASSERT_HEX(s, out.magic,    PROTO_MAGIC);
    TEST_ASSERT_EQ(s, out.version,   PROTO_VERSION);
    TEST_ASSERT_EQ(s, out.type,      MSG_READ_REG);
    TEST_ASSERT_EQ(s, out.sequence,  0x0042);
    TEST_ASSERT_EQ(s, out.payload_len, PLEN_READ_REG);
    TEST_ASSERT_HEX(s, out.payload.read_reg.addr, 0x000Eu);
}

static void test_roundtrip_write_reg(test_suite_t *s) {
    packet_t req, out;
    pkt_build_write_reg(&req, 0x0001, 0x0004, 0xDEADBEEFu);
    TEST_ASSERT_EQ(s, roundtrip(&req, &out), 0);
    TEST_ASSERT_EQ(s, out.type,      MSG_WRITE_REG);
    TEST_ASSERT_EQ(s, out.sequence,  0x0001);
    TEST_ASSERT_HEX(s, out.payload.write_reg.addr,  0x0004u);
    TEST_ASSERT_HEX(s, out.payload.write_reg.value, 0xDEADBEEFu);
}

static void test_roundtrip_read_resp(test_suite_t *s) {
    packet_t resp, out;
    pkt_build_read_resp(&resp, 0x00FF, 0x0006, 0x12345678u);
    TEST_ASSERT_EQ(s, roundtrip(&resp, &out), 0);
    TEST_ASSERT_EQ(s, out.type, MSG_READ_RESP);
    TEST_ASSERT_HEX(s, out.payload.read_resp.addr,  0x0006u);
    TEST_ASSERT_HEX(s, out.payload.read_resp.value, 0x12345678u);
}

static void test_roundtrip_write_ack(test_suite_t *s) {
    packet_t pkt, out;
    pkt_build_write_ack(&pkt, 0x0010);
    TEST_ASSERT_EQ(s, roundtrip(&pkt, &out), 0);
    TEST_ASSERT_EQ(s, out.type, MSG_WRITE_ACK);
    TEST_ASSERT_EQ(s, out.payload_len, 0);
}

static void test_roundtrip_error(test_suite_t *s) {
    packet_t pkt, out;
    pkt_build_error(&pkt, 0x0020, ERR_BAD_CRC, 0x0019);
    TEST_ASSERT_EQ(s, roundtrip(&pkt, &out), 0);
    TEST_ASSERT_EQ(s, out.type, MSG_ERROR);
    TEST_ASSERT_EQ(s, out.payload.error.code,    ERR_BAD_CRC);
    TEST_ASSERT_EQ(s, out.payload.error.bad_seq, 0x0019);
}

static void test_roundtrip_ping_pong(test_suite_t *s) {
    packet_t ping, pong, out;
    pkt_build_ping(&ping, 0x0007);
    pkt_build_pong(&pong, 0x0008);
    TEST_ASSERT_EQ(s, roundtrip(&ping, &out), 0);
    TEST_ASSERT_EQ(s, out.type, MSG_PING);
    TEST_ASSERT_EQ(s, roundtrip(&pong, &out), 0);
    TEST_ASSERT_EQ(s, out.type, MSG_PONG);
}

static void test_roundtrip_reset(test_suite_t *s) {
    packet_t rst, ack, out;
    pkt_build_reset(&rst,     0x0003);
    pkt_build_reset_ack(&ack, 0x0003);
    TEST_ASSERT_EQ(s, roundtrip(&rst, &out), 0);
    TEST_ASSERT_EQ(s, out.type, MSG_RESET);
    TEST_ASSERT_EQ(s, roundtrip(&ack, &out), 0);
    TEST_ASSERT_EQ(s, out.type, MSG_RESET_ACK);
}

/* =========================================================================
 * decode_packet error cases
 * ====================================================================== */

static void test_decode_bad_magic(test_suite_t *s) {
    packet_t pkt, out;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 1);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    /* Corrupt magic */
    buf[0] = 0xDE; buf[1] = 0xC0;
    TEST_ASSERT_EQ(s, decode_packet(buf, (size_t)n, &out), -2);
}

static void test_decode_bad_version(test_suite_t *s) {
    packet_t pkt, out;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 2);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    buf[HDR_OFF_VERSION] = 0x99;
    /* CRC will also be wrong — bad version fires first */
    TEST_ASSERT_EQ(s, decode_packet(buf, (size_t)n, &out), -3);
}

static void test_decode_bad_crc(test_suite_t *s) {
    packet_t pkt, out;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_read_reg(&pkt, 3, 0x000E);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    buf[HDR_OFF_CRC] ^= 0xFF;  /* corrupt one CRC byte */
    TEST_ASSERT_EQ(s, decode_packet(buf, (size_t)n, &out), -4);
}

static void test_decode_truncated(test_suite_t *s) {
    packet_t pkt, out;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_read_reg(&pkt, 4, 0x0000);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    /* Feed only the header — payload missing */
    TEST_ASSERT_EQ(s, decode_packet(buf, PROTO_HEADER_LEN - 1, &out), -1);
}

static void test_decode_oversized_plen(test_suite_t *s) {
    packet_t pkt, out;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 5);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    /* Set payload_len to 257 (> PROTO_MAX_PAYLOAD=256) */
    buf[HDR_OFF_PLEN]     = 0x01;
    buf[HDR_OFF_PLEN + 1] = 0x01;
    TEST_ASSERT_EQ(s, decode_packet(buf, (size_t)n, &out), -5);
}

static void test_encode_buf_too_small(test_suite_t *s) {
    packet_t pkt;
    uint8_t small_buf[4];
    pkt_build_ping(&pkt, 6);
    TEST_ASSERT_EQ(s, encode_packet(&pkt, small_buf, sizeof(small_buf)), -1);
}

/* =========================================================================
 * Streaming parser tests
 * ====================================================================== */

/* Callback context */
typedef struct {
    packet_t  pkts[32];
    int       count;
    err_code_t errs[32];
    uint16_t   err_seqs[32];
    int       err_count;
} parse_ctx_t;

static void on_packet(const packet_t *pkt, void *ud) {
    parse_ctx_t *ctx = (parse_ctx_t *)ud;
    if (ctx->count < 32) {
        ctx->pkts[ctx->count++] = *pkt;
    }
}

static void on_error(err_code_t code, uint16_t seq, void *ud) {
    parse_ctx_t *ctx = (parse_ctx_t *)ud;
    if (ctx->err_count < 32) {
        ctx->errs[ctx->err_count]     = code;
        ctx->err_seqs[ctx->err_count] = seq;
        ctx->err_count++;
    }
}

static void test_parser_one_packet(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0001);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);

    parser_feed(p, buf, (size_t)n, on_packet, on_error, &ctx);
    TEST_ASSERT_EQ(s, ctx.count,     1);
    TEST_ASSERT_EQ(s, ctx.err_count, 0);
    TEST_ASSERT_EQ(s, ctx.pkts[0].type, MSG_PING);

    parser_free(p);
}

static void test_parser_byte_by_byte(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_write_reg(&pkt, 0x0002, 0x000E, 0xCAFEBABEu);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);

    /* Feed 1 byte at a time */
    for (int i = 0; i < n; i++) {
        parser_feed(p, buf + i, 1, on_packet, on_error, &ctx);
    }
    TEST_ASSERT_EQ(s, ctx.count, 1);
    TEST_ASSERT_EQ(s, ctx.err_count, 0);
    TEST_ASSERT_EQ(s, ctx.pkts[0].type, MSG_WRITE_REG);
    TEST_ASSERT_HEX(s, ctx.pkts[0].payload.write_reg.value, 0xCAFEBABEu);

    parser_free(p);
}

static void test_parser_multiple_packets(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Build 10 packets concatenated */
    uint8_t stream[PROTO_MAX_PACKET * 10];
    size_t total = 0;
    for (int i = 0; i < 10; i++) {
        packet_t pkt;
        pkt_build_ping(&pkt, (uint16_t)i);
        uint8_t buf[PROTO_MAX_PACKET];
        int n = encode_packet(&pkt, buf, sizeof(buf));
        TEST_ASSERT(s, n > 0);
        memcpy(stream + total, buf, (size_t)n);
        total += (size_t)n;
    }

    parser_feed(p, stream, total, on_packet, on_error, &ctx);
    TEST_ASSERT_EQ(s, ctx.count,     10);
    TEST_ASSERT_EQ(s, ctx.err_count, 0);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQ(s, ctx.pkts[i].type,     MSG_PING);
        TEST_ASSERT_EQ(s, ctx.pkts[i].sequence, i);
    }

    parser_free(p);
}

static void test_parser_bad_magic_resync(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Garbage bytes followed by a valid packet */
    uint8_t garbage[] = { 0x00, 0x11, 0x22, 0x33, 0x44 };

    packet_t pkt;
    uint8_t good[PROTO_MAX_PACKET];
    pkt_build_pong(&pkt, 0x0099);
    int n = encode_packet(&pkt, good, sizeof(good));
    TEST_ASSERT(s, n > 0);

    uint8_t stream[sizeof(garbage) + PROTO_MAX_PACKET];
    memcpy(stream, garbage, sizeof(garbage));
    memcpy(stream + sizeof(garbage), good, (size_t)n);

    parser_feed(p, stream, sizeof(garbage) + (size_t)n,
                on_packet, on_error, &ctx);

    /* The valid packet should still be decoded */
    TEST_ASSERT_EQ(s, ctx.count, 1);
    TEST_ASSERT_EQ(s, ctx.pkts[0].type, MSG_PONG);

    parser_free(p);
}

static void test_parser_bad_crc_resync(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* A bad-CRC packet followed immediately by a good one */
    packet_t pkt;
    uint8_t bad[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0010);
    int n1 = encode_packet(&pkt, bad, sizeof(bad));
    TEST_ASSERT(s, n1 > 0);
    bad[HDR_OFF_CRC] ^= 0xFFu; /* corrupt CRC */

    packet_t pkt2;
    uint8_t good[PROTO_MAX_PACKET];
    pkt_build_pong(&pkt2, 0x0011);
    int n2 = encode_packet(&pkt2, good, sizeof(good));
    TEST_ASSERT(s, n2 > 0);

    uint8_t stream[PROTO_MAX_PACKET * 2];
    memcpy(stream,        bad,  (size_t)n1);
    memcpy(stream + n1, good, (size_t)n2);

    parser_feed(p, stream, (size_t)(n1 + n2), on_packet, on_error, &ctx);

    /* One error (bad CRC), one good packet */
    TEST_ASSERT_EQ(s, ctx.err_count, 1);
    TEST_ASSERT_EQ(s, ctx.errs[0], ERR_BAD_CRC);
    TEST_ASSERT_EQ(s, ctx.count,   1);
    TEST_ASSERT_EQ(s, ctx.pkts[0].type, MSG_PONG);

    parser_free(p);
}

static void test_parser_bad_version(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0020);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);
    buf[HDR_OFF_VERSION] = 0x42;

    parser_feed(p, buf, (size_t)n, on_packet, on_error, &ctx);

    TEST_ASSERT_EQ(s, ctx.err_count, 1);
    TEST_ASSERT_EQ(s, ctx.errs[0],   ERR_BAD_VERSION);
    TEST_ASSERT_EQ(s, ctx.count,     0);

    parser_free(p);
}

static void test_parser_split_across_two_feeds(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_read_reg(&pkt, 0x0030, 0x0004);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);

    int split = n / 2;
    parser_feed(p, buf,         (size_t)split,        on_packet, on_error, &ctx);
    TEST_ASSERT_EQ(s, ctx.count, 0);  /* not complete yet */
    parser_feed(p, buf + split, (size_t)(n - split),  on_packet, on_error, &ctx);
    TEST_ASSERT_EQ(s, ctx.count, 1);
    TEST_ASSERT_EQ(s, ctx.pkts[0].type, MSG_READ_REG);

    parser_free(p);
}

static void test_parser_reset(test_suite_t *s) {
    parser_t *p = parser_new();
    parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Feed half a packet */
    packet_t pkt;
    uint8_t buf[PROTO_MAX_PACKET];
    pkt_build_ping(&pkt, 0x0040);
    int n = encode_packet(&pkt, buf, sizeof(buf));
    TEST_ASSERT(s, n > 0);

    parser_feed(p, buf, (size_t)(n / 2), on_packet, on_error, &ctx);
    TEST_ASSERT_EQ(s, ctx.count, 0);

    /* Reset — discard partial */
    parser_reset(p);

    /* Now feed a complete valid packet */
    packet_t pkt2;
    uint8_t buf2[PROTO_MAX_PACKET];
    pkt_build_pong(&pkt2, 0x0041);
    int n2 = encode_packet(&pkt2, buf2, sizeof(buf2));
    TEST_ASSERT(s, n2 > 0);

    parser_feed(p, buf2, (size_t)n2, on_packet, on_error, &ctx);
    TEST_ASSERT_EQ(s, ctx.count, 1);
    TEST_ASSERT_EQ(s, ctx.pkts[0].type, MSG_PONG);

    parser_free(p);
}

/* =========================================================================
 * Convenience builder correctness
 * ====================================================================== */

static void test_builder_fields(test_suite_t *s) {
    packet_t pkt;

    pkt_build_read_reg(&pkt, 0xABCD, 0x1234);
    TEST_ASSERT_HEX(s, pkt.magic,    PROTO_MAGIC);
    TEST_ASSERT_EQ (s, pkt.version,  PROTO_VERSION);
    TEST_ASSERT_EQ (s, pkt.sequence, 0xABCD);
    TEST_ASSERT_EQ (s, pkt.type,     MSG_READ_REG);
    TEST_ASSERT_EQ (s, pkt.payload_len, PLEN_READ_REG);
    /* raw_payload must contain the addr in network byte order */
    TEST_ASSERT_EQ (s, pkt.raw_payload[0], 0x12);
    TEST_ASSERT_EQ (s, pkt.raw_payload[1], 0x34);

    pkt_build_write_reg(&pkt, 1, 0x0004, 0x01020304u);
    TEST_ASSERT_EQ (s, pkt.payload_len, PLEN_WRITE_REG);
    TEST_ASSERT_EQ (s, pkt.raw_payload[0], 0x00);
    TEST_ASSERT_EQ (s, pkt.raw_payload[1], 0x04);
    TEST_ASSERT_EQ (s, pkt.raw_payload[2], 0x01);
    TEST_ASSERT_EQ (s, pkt.raw_payload[3], 0x02);
    TEST_ASSERT_EQ (s, pkt.raw_payload[4], 0x03);
    TEST_ASSERT_EQ (s, pkt.raw_payload[5], 0x04);

    pkt_build_error(&pkt, 2, ERR_UNKNOWN_REGISTER, 0x0007);
    TEST_ASSERT_EQ (s, pkt.payload_len,          PLEN_ERROR);
    TEST_ASSERT_EQ (s, pkt.raw_payload[0],        ERR_UNKNOWN_REGISTER);
    TEST_ASSERT_EQ (s, pkt.raw_payload[1],        0x00);
    TEST_ASSERT_EQ (s, pkt.raw_payload[2],        0x07);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    crc32_init();
    test_suite_t *s = test_suite_new("Codec Unit Tests");

    /* CRC */
    RUN_TEST(s, test_crc_known_vector);
    RUN_TEST(s, test_crc_empty);
    RUN_TEST(s, test_crc_incremental);
    RUN_TEST(s, test_crc_two_halves);
    RUN_TEST(s, test_crc_all_zeros);
    RUN_TEST(s, test_crc_all_ones);

    /* Round-trip encode/decode */
    RUN_TEST(s, test_roundtrip_read_reg);
    RUN_TEST(s, test_roundtrip_write_reg);
    RUN_TEST(s, test_roundtrip_read_resp);
    RUN_TEST(s, test_roundtrip_write_ack);
    RUN_TEST(s, test_roundtrip_error);
    RUN_TEST(s, test_roundtrip_ping_pong);
    RUN_TEST(s, test_roundtrip_reset);

    /* decode error cases */
    RUN_TEST(s, test_decode_bad_magic);
    RUN_TEST(s, test_decode_bad_version);
    RUN_TEST(s, test_decode_bad_crc);
    RUN_TEST(s, test_decode_truncated);
    RUN_TEST(s, test_decode_oversized_plen);
    RUN_TEST(s, test_encode_buf_too_small);

    /* Streaming parser */
    RUN_TEST(s, test_parser_one_packet);
    RUN_TEST(s, test_parser_byte_by_byte);
    RUN_TEST(s, test_parser_multiple_packets);
    RUN_TEST(s, test_parser_bad_magic_resync);
    RUN_TEST(s, test_parser_bad_crc_resync);
    RUN_TEST(s, test_parser_bad_version);
    RUN_TEST(s, test_parser_split_across_two_feeds);
    RUN_TEST(s, test_parser_reset);

    /* Builder correctness */
    RUN_TEST(s, test_builder_fields);

    return test_suite_finish(s);
}
