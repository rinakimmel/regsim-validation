/*
 * protocol.h — Wire protocol definitions for the Register Simulator.
 *
 * Binary protocol over TCP (or UDP), network byte order.
 * Fixed 12-byte header followed by 0..256 bytes of payload.
 *
 * Header layout:
 *   [0..1]  magic          0xC0DE
 *   [2]     version        1
 *   [3]     type           MSG_TYPE_*
 *   [4..5]  sequence       request/response pair id
 *   [6..7]  payload_len    0..256
 *   [8..11] crc32          IEEE 802.3 reflected, over header(crc=0)+payload
 */

#ifndef REGSIM_PROTOCOL_H
#define REGSIM_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Protocol constants
 * ---------------------------------------------------------------------- */

#define PROTO_MAGIC          UINT16_C(0xC0DE)
#define PROTO_VERSION        UINT8_C(1)

#define PROTO_HEADER_LEN     12u          /* bytes */
#define PROTO_MAX_PAYLOAD    256u         /* bytes */
#define PROTO_MAX_PACKET     (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD)

/* Byte offsets within the raw header buffer */
#define HDR_OFF_MAGIC        0
#define HDR_OFF_VERSION      2
#define HDR_OFF_TYPE         3
#define HDR_OFF_SEQ          4
#define HDR_OFF_PLEN         6
#define HDR_OFF_CRC          8

/* -------------------------------------------------------------------------
 * Message types
 * ---------------------------------------------------------------------- */

typedef enum {
    MSG_READ_REG    = 0x01,
    MSG_WRITE_REG   = 0x02,
    MSG_READ_RESP   = 0x03,
    MSG_WRITE_ACK   = 0x04,
    MSG_ERROR       = 0x05,
    MSG_PING        = 0x06,
    MSG_PONG        = 0x07,
    MSG_RESET       = 0x08,
    MSG_RESET_ACK   = 0x09,
} msg_type_t;

/* -------------------------------------------------------------------------
 * Error codes (payload byte 0 of MSG_ERROR)
 * ---------------------------------------------------------------------- */

typedef enum {
    ERR_BAD_MAGIC          = 0x01,
    ERR_BAD_VERSION        = 0x02,
    ERR_BAD_CRC            = 0x03,
    ERR_UNKNOWN_TYPE       = 0x04,
    ERR_UNKNOWN_REGISTER   = 0x05,
    ERR_READ_ONLY_VIOLATION= 0x06,
    ERR_INVALID_LENGTH     = 0x07,
    ERR_INTERNAL           = 0x08,
} err_code_t;

/* -------------------------------------------------------------------------
 * Payload sizes (fixed for each message type)
 * ---------------------------------------------------------------------- */

#define PLEN_READ_REG        2u   /* addr(2) */
#define PLEN_WRITE_REG       6u   /* addr(2) + value(4) */
#define PLEN_READ_RESP       6u   /* addr(2) + value(4) */
#define PLEN_WRITE_ACK       0u
#define PLEN_ERROR           3u   /* code(1) + offending_seq(2) */
#define PLEN_PING            0u
#define PLEN_PONG            0u
#define PLEN_RESET           0u
#define PLEN_RESET_ACK       0u

/* -------------------------------------------------------------------------
 * Decoded packet structure (host byte order)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;          /* msg_type_t */
    uint16_t sequence;
    uint16_t payload_len;
    uint32_t crc32;

    /* Decoded payload fields — only the union member matching `type` is valid */
    union {
        struct { uint16_t addr;                   } read_reg;
        struct { uint16_t addr; uint32_t value;   } write_reg;
        struct { uint16_t addr; uint32_t value;   } read_resp;
        struct { uint8_t  code; uint16_t bad_seq; } error;
    } payload;

    /* Raw payload bytes (for forwarding / logging) */
    uint8_t raw_payload[PROTO_MAX_PAYLOAD];
} packet_t;

/* -------------------------------------------------------------------------
 * Encode / Decode API
 * ---------------------------------------------------------------------- */

/*
 * encode_packet — serialise *pkt into buf[0..buf_len).
 *
 * Returns the number of bytes written (PROTO_HEADER_LEN + payload_len),
 * or -1 if buf_len is too small or payload_len > PROTO_MAX_PAYLOAD.
 *
 * The CRC is computed and written into buf; pkt->crc32 is updated.
 */
int encode_packet(const packet_t *pkt, uint8_t *buf, size_t buf_len);

/*
 * decode_packet — deserialise buf[0..buf_len) into *pkt.
 *
 * Returns 0 on success.
 * Returns -1 on truncated buffer (buf_len < PROTO_HEADER_LEN + payload_len).
 * Returns -2 on bad magic.
 * Returns -3 on bad version.
 * Returns -4 on CRC mismatch.
 * Returns -5 on payload_len > PROTO_MAX_PAYLOAD.
 */
int decode_packet(const uint8_t *buf, size_t buf_len, packet_t *pkt);

/* -------------------------------------------------------------------------
 * Streaming parser
 * ---------------------------------------------------------------------- */

/* Opaque parser state — embed in a per-connection struct */
typedef struct parser parser_t;

/* Allocate and initialise a new parser */
parser_t *parser_new(void);

/* Free a parser */
void parser_free(parser_t *p);

/*
 * Callback invoked by parser_feed for each complete, validated packet.
 * Return value is currently ignored.
 */
typedef void (*parser_cb_t)(const packet_t *pkt, void *userdata);

/*
 * parser_feed — push `len` bytes into the parser.
 *
 * For each complete, CRC-valid packet found, cb is invoked.
 * On a bad CRC or bad header the parser emits an error token (err_cb, if
 * supplied) and scans forward for the next magic bytes.
 *
 * err_cb may be NULL.  err_seq is the sequence number recovered from the
 * bad header (0 if the header itself was unreadable).
 */
typedef void (*parser_err_cb_t)(err_code_t code, uint16_t err_seq,
                                void *userdata);

void parser_feed(parser_t *p, const uint8_t *data, size_t len,
                 parser_cb_t cb, parser_err_cb_t err_cb, void *userdata);

/*
 * parser_reset — discard all buffered bytes and reset to SYNC state.
 */
void parser_reset(parser_t *p);

/* -------------------------------------------------------------------------
 * Convenience builders
 * ---------------------------------------------------------------------- */

/* Fill *pkt for a READ_REG request */
void pkt_build_read_reg(packet_t *pkt, uint16_t seq, uint16_t addr);

/* Fill *pkt for a WRITE_REG request */
void pkt_build_write_reg(packet_t *pkt, uint16_t seq,
                         uint16_t addr, uint32_t value);

/* Fill *pkt for a READ_RESP response */
void pkt_build_read_resp(packet_t *pkt, uint16_t seq,
                         uint16_t addr, uint32_t value);

/* Fill *pkt for a WRITE_ACK response */
void pkt_build_write_ack(packet_t *pkt, uint16_t seq);

/* Fill *pkt for an ERROR response */
void pkt_build_error(packet_t *pkt, uint16_t seq,
                     err_code_t code, uint16_t bad_seq);

/* Fill *pkt for PING / PONG */
void pkt_build_ping(packet_t *pkt, uint16_t seq);
void pkt_build_pong(packet_t *pkt, uint16_t seq);

/* Fill *pkt for RESET / RESET_ACK */
void pkt_build_reset(packet_t *pkt, uint16_t seq);
void pkt_build_reset_ack(packet_t *pkt, uint16_t seq);

/* Human-readable name for a message type (for logging) */
const char *msg_type_name(uint8_t type);

/* Human-readable name for an error code */
const char *err_code_name(uint8_t code);

#endif /* REGSIM_PROTOCOL_H */
