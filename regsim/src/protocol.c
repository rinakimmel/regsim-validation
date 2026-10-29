/*
 * protocol.c — Packet encode/decode and streaming parser.
 *
 * All multi-byte fields are serialised explicitly in network byte order
 * (big-endian) using individual byte assignments — no struct casting,
 * no memcpy of typed structs, no reliance on host endianness.
 */

#include "protocol.h"
#include "crc32.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Byte-order helpers (no htons/htonl dependency on byte width)
 * ---------------------------------------------------------------------- */

static inline void put_u8(uint8_t *buf, uint8_t v) {
    buf[0] = v;
}

static inline void put_u16_be(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)((v >> 8) & 0xFFu);
    buf[1] = (uint8_t)( v       & 0xFFu);
}

static inline void put_u32_be(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)((v >> 24) & 0xFFu);
    buf[1] = (uint8_t)((v >> 16) & 0xFFu);
    buf[2] = (uint8_t)((v >>  8) & 0xFFu);
    buf[3] = (uint8_t)( v        & 0xFFu);
}

static inline uint8_t get_u8(const uint8_t *buf) {
    return buf[0];
}

static inline uint16_t get_u16_be(const uint8_t *buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
}

static inline uint32_t get_u32_be(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24)
         | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] <<  8)
         |  (uint32_t)buf[3];
}

/* -------------------------------------------------------------------------
 * CRC computation over a packet buffer
 * (header with CRC field zeroed) + payload
 * ---------------------------------------------------------------------- */

static uint32_t compute_crc(const uint8_t *buf, uint16_t payload_len) {
    /* Copy header, zero the CRC field, then feed header + payload */
    uint8_t hdr_copy[PROTO_HEADER_LEN];
    memcpy(hdr_copy, buf, PROTO_HEADER_LEN);
    hdr_copy[HDR_OFF_CRC + 0] = 0;
    hdr_copy[HDR_OFF_CRC + 1] = 0;
    hdr_copy[HDR_OFF_CRC + 2] = 0;
    hdr_copy[HDR_OFF_CRC + 3] = 0;

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, hdr_copy, PROTO_HEADER_LEN);
    if (payload_len > 0) {
        crc = crc32_update(crc, buf + PROTO_HEADER_LEN, payload_len);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* -------------------------------------------------------------------------
 * encode_packet
 * ---------------------------------------------------------------------- */

int encode_packet(const packet_t *pkt, uint8_t *buf, size_t buf_len) {
    if (!pkt || !buf) return -1;
    if (pkt->payload_len > PROTO_MAX_PAYLOAD) return -1;

    size_t total = PROTO_HEADER_LEN + pkt->payload_len;
    if (buf_len < total) return -1;

    put_u16_be(buf + HDR_OFF_MAGIC,   pkt->magic);
    put_u8    (buf + HDR_OFF_VERSION, pkt->version);
    put_u8    (buf + HDR_OFF_TYPE,    pkt->type);
    put_u16_be(buf + HDR_OFF_SEQ,     pkt->sequence);
    put_u16_be(buf + HDR_OFF_PLEN,    pkt->payload_len);
    /* Zero CRC field before computing */
    put_u32_be(buf + HDR_OFF_CRC,     0u);

    /* Copy raw payload */
    if (pkt->payload_len > 0) {
        memcpy(buf + PROTO_HEADER_LEN, pkt->raw_payload, pkt->payload_len);
    }

    /* Compute and write CRC */
    uint32_t crc = compute_crc(buf, pkt->payload_len);
    put_u32_be(buf + HDR_OFF_CRC, crc);

    /* Update the packet struct so caller can inspect the CRC */
    ((packet_t *)pkt)->crc32 = crc;

    return (int)total;
}

/* -------------------------------------------------------------------------
 * decode_packet
 * ---------------------------------------------------------------------- */

int decode_packet(const uint8_t *buf, size_t buf_len, packet_t *pkt) {
    if (!buf || !pkt) return -1;
    if (buf_len < PROTO_HEADER_LEN) return -1;

    pkt->magic       = get_u16_be(buf + HDR_OFF_MAGIC);
    pkt->version     = get_u8    (buf + HDR_OFF_VERSION);
    pkt->type        = get_u8    (buf + HDR_OFF_TYPE);
    pkt->sequence    = get_u16_be(buf + HDR_OFF_SEQ);
    pkt->payload_len = get_u16_be(buf + HDR_OFF_PLEN);
    pkt->crc32       = get_u32_be(buf + HDR_OFF_CRC);

    if (pkt->magic != PROTO_MAGIC)         return -2;
    if (pkt->version != PROTO_VERSION)     return -3;
    if (pkt->payload_len > PROTO_MAX_PAYLOAD) return -5;

    size_t total = PROTO_HEADER_LEN + pkt->payload_len;
    if (buf_len < total)                   return -1;

    /* Verify CRC */
    uint32_t expected = compute_crc(buf, pkt->payload_len);
    if (pkt->crc32 != expected)            return -4;

    /* Copy raw payload */
    if (pkt->payload_len > 0) {
        memcpy(pkt->raw_payload, buf + PROTO_HEADER_LEN, pkt->payload_len);
    }

    /* Decode payload fields by type */
    switch ((msg_type_t)pkt->type) {
    case MSG_READ_REG:
        if (pkt->payload_len >= 2) {
            pkt->payload.read_reg.addr = get_u16_be(pkt->raw_payload);
        }
        break;
    case MSG_WRITE_REG:
        if (pkt->payload_len >= 6) {
            pkt->payload.write_reg.addr  = get_u16_be(pkt->raw_payload);
            pkt->payload.write_reg.value = get_u32_be(pkt->raw_payload + 2);
        }
        break;
    case MSG_READ_RESP:
        if (pkt->payload_len >= 6) {
            pkt->payload.read_resp.addr  = get_u16_be(pkt->raw_payload);
            pkt->payload.read_resp.value = get_u32_be(pkt->raw_payload + 2);
        }
        break;
    case MSG_ERROR:
        if (pkt->payload_len >= 3) {
            pkt->payload.error.code    = get_u8    (pkt->raw_payload);
            pkt->payload.error.bad_seq = get_u16_be(pkt->raw_payload + 1);
        }
        break;
    case MSG_WRITE_ACK:
    case MSG_PING:
    case MSG_PONG:
    case MSG_RESET:
    case MSG_RESET_ACK:
        /* No payload fields to decode */
        break;
    default:
        break;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Streaming parser
 * ---------------------------------------------------------------------- */

typedef enum {
    PS_SYNC,        /* scanning for 0xC0 */
    PS_SYNC2,       /* saw 0xC0, waiting for 0xDE */
    PS_HEADER,      /* accumulating header bytes */
    PS_PAYLOAD,     /* accumulating payload bytes */
} parser_state_t;

struct parser {
    parser_state_t state;
    uint8_t  buf[PROTO_MAX_PACKET];   /* working buffer */
    size_t   buf_pos;                 /* bytes written so far */
    uint16_t payload_len;             /* decoded from header */
};

parser_t *parser_new(void) {
    parser_t *p = (parser_t *)calloc(1, sizeof(parser_t));
    return p;
}

void parser_free(parser_t *p) {
    free(p);
}

void parser_reset(parser_t *p) {
    if (!p) return;
    p->state   = PS_SYNC;
    p->buf_pos = 0;
    p->payload_len = 0;
}

/*
 * _parser_emit_error — try to extract sequence number from the buffered
 * (possibly partial) header and fire err_cb.
 */
static void _parser_emit_error(parser_t *p, err_code_t code,
                                parser_err_cb_t err_cb, void *ud) {
    if (!err_cb) return;
    uint16_t seq = 0;
    if (p->buf_pos > (size_t)HDR_OFF_SEQ + 1u) {
        seq = get_u16_be(p->buf + HDR_OFF_SEQ);
    }
    err_cb(code, seq, ud);
}

/*
 * _parser_resync — called after a bad packet.  Scans buf[1..buf_pos) for
 * the magic bytes 0xC0 0xDE and restores parser state.
 */
static void _parser_resync(parser_t *p) {
    /* Search for 0xC0 0xDE starting at byte 1 (byte 0 was the bad start) */
    size_t found = 0;
    for (size_t i = 1; i < p->buf_pos; i++) {
        if (p->buf[i] == 0xC0u) {
            /* Potential magic start — slide buffer */
            size_t remaining = p->buf_pos - i;
            memmove(p->buf, p->buf + i, remaining);
            p->buf_pos = remaining;
            p->state   = PS_SYNC2;   /* already have 0xC0 */
            found = 1;
            break;
        }
    }
    if (!found) {
        p->buf_pos = 0;
        p->state   = PS_SYNC;
    }
}

void parser_feed(parser_t *p, const uint8_t *data, size_t len,
                 parser_cb_t cb, parser_err_cb_t err_cb, void *userdata) {
    for (size_t i = 0; i < len; ) {
        uint8_t byte = data[i];

        switch (p->state) {
        case PS_SYNC:
            if (byte == 0xC0u) {
                p->buf[0] = byte;
                p->buf_pos = 1;
                p->state = PS_SYNC2;
            }
            i++;
            break;

        case PS_SYNC2:
            if (byte == 0xDEu) {
                p->buf[1] = byte;
                p->buf_pos = 2;
                p->state = PS_HEADER;
            } else if (byte == 0xC0u) {
                /* Another 0xC0 — restart with this byte */
                p->buf[0] = byte;
                p->buf_pos = 1;
                /* state stays PS_SYNC2 */
            } else {
                p->buf_pos = 0;
                p->state = PS_SYNC;
            }
            i++;
            break;

        case PS_HEADER: {
            /* Accumulate until we have PROTO_HEADER_LEN bytes */
            size_t need = PROTO_HEADER_LEN - p->buf_pos;
            size_t avail = len - i;
            size_t take = (avail < need) ? avail : need;
            memcpy(p->buf + p->buf_pos, data + i, take);
            p->buf_pos += take;
            i += take;

            if (p->buf_pos < PROTO_HEADER_LEN) {
                break; /* need more bytes */
            }

            /* We have a full header — validate magic/version/plen */
            uint16_t magic = get_u16_be(p->buf + HDR_OFF_MAGIC);
            uint8_t  ver   = get_u8    (p->buf + HDR_OFF_VERSION);
            uint16_t plen  = get_u16_be(p->buf + HDR_OFF_PLEN);

            if (magic != PROTO_MAGIC) {
                _parser_emit_error(p, ERR_BAD_MAGIC, err_cb, userdata);
                _parser_resync(p);
                break;
            }
            if (ver != PROTO_VERSION) {
                _parser_emit_error(p, ERR_BAD_VERSION, err_cb, userdata);
                _parser_resync(p);
                break;
            }
            if (plen > PROTO_MAX_PAYLOAD) {
                _parser_emit_error(p, ERR_INVALID_LENGTH, err_cb, userdata);
                _parser_resync(p);
                break;
            }

            p->payload_len = plen;
            if (plen == 0) {
                /* No payload — verify CRC immediately */
                uint32_t got_crc0 = get_u32_be(p->buf + HDR_OFF_CRC);
                uint32_t exp_crc0 = compute_crc(p->buf, 0);
                if (got_crc0 != exp_crc0) {
                    _parser_emit_error(p, ERR_BAD_CRC, err_cb, userdata);
                    _parser_resync(p);
                    break;
                }
                if (cb) {
                    packet_t pkt;
                    (void)decode_packet(p->buf, PROTO_HEADER_LEN, &pkt);
                    cb(&pkt, userdata);
                }
                p->buf_pos = 0;
                p->state   = PS_SYNC;
                break;
            }
            p->state = PS_PAYLOAD;
            break;
        }

        case PS_PAYLOAD: {
            size_t need  = (size_t)p->payload_len - (p->buf_pos - PROTO_HEADER_LEN);
            size_t avail = len - i;
            size_t take  = (avail < need) ? avail : need;
            memcpy(p->buf + p->buf_pos, data + i, take);
            p->buf_pos += take;
            i += take;

            size_t want = PROTO_HEADER_LEN + (size_t)p->payload_len;
            if (p->buf_pos < want) {
                break; /* need more bytes */
            }

            /* Full packet — verify CRC */
            uint32_t got_crc = get_u32_be(p->buf + HDR_OFF_CRC);
            uint32_t exp_crc = compute_crc(p->buf, p->payload_len);
            if (got_crc != exp_crc) {
                _parser_emit_error(p, ERR_BAD_CRC, err_cb, userdata);
                _parser_resync(p);
                break;
            }
            if (cb) {
                packet_t pkt;
                (void)decode_packet(p->buf, want, &pkt);
                cb(&pkt, userdata);
            }
            p->buf_pos = 0;
            p->state   = PS_SYNC;
            break;
        }
        } /* switch */
    } /* for */
}

/* -------------------------------------------------------------------------
 * Convenience builders
 * ---------------------------------------------------------------------- */

static void _fill_base(packet_t *pkt, uint8_t type, uint16_t seq,
                       uint16_t payload_len) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->magic       = PROTO_MAGIC;
    pkt->version     = PROTO_VERSION;
    pkt->type        = type;
    pkt->sequence    = seq;
    pkt->payload_len = payload_len;
}

void pkt_build_read_reg(packet_t *pkt, uint16_t seq, uint16_t addr) {
    _fill_base(pkt, MSG_READ_REG, seq, PLEN_READ_REG);
    pkt->payload.read_reg.addr = addr;
    put_u16_be(pkt->raw_payload, addr);
}

void pkt_build_write_reg(packet_t *pkt, uint16_t seq,
                         uint16_t addr, uint32_t value) {
    _fill_base(pkt, MSG_WRITE_REG, seq, PLEN_WRITE_REG);
    pkt->payload.write_reg.addr  = addr;
    pkt->payload.write_reg.value = value;
    put_u16_be(pkt->raw_payload,     addr);
    put_u32_be(pkt->raw_payload + 2, value);
}

void pkt_build_read_resp(packet_t *pkt, uint16_t seq,
                         uint16_t addr, uint32_t value) {
    _fill_base(pkt, MSG_READ_RESP, seq, PLEN_READ_RESP);
    pkt->payload.read_resp.addr  = addr;
    pkt->payload.read_resp.value = value;
    put_u16_be(pkt->raw_payload,     addr);
    put_u32_be(pkt->raw_payload + 2, value);
}

void pkt_build_write_ack(packet_t *pkt, uint16_t seq) {
    _fill_base(pkt, MSG_WRITE_ACK, seq, PLEN_WRITE_ACK);
}

void pkt_build_error(packet_t *pkt, uint16_t seq,
                     err_code_t code, uint16_t bad_seq) {
    _fill_base(pkt, MSG_ERROR, seq, PLEN_ERROR);
    pkt->payload.error.code    = (uint8_t)code;
    pkt->payload.error.bad_seq = bad_seq;
    put_u8    (pkt->raw_payload,     (uint8_t)code);
    put_u16_be(pkt->raw_payload + 1, bad_seq);
}

void pkt_build_ping(packet_t *pkt, uint16_t seq) {
    _fill_base(pkt, MSG_PING, seq, PLEN_PING);
}

void pkt_build_pong(packet_t *pkt, uint16_t seq) {
    _fill_base(pkt, MSG_PONG, seq, PLEN_PONG);
}

void pkt_build_reset(packet_t *pkt, uint16_t seq) {
    _fill_base(pkt, MSG_RESET, seq, PLEN_RESET);
}

void pkt_build_reset_ack(packet_t *pkt, uint16_t seq) {
    _fill_base(pkt, MSG_RESET_ACK, seq, PLEN_RESET_ACK);
}

/* -------------------------------------------------------------------------
 * Name helpers
 * ---------------------------------------------------------------------- */

const char *msg_type_name(uint8_t type) {
    switch ((msg_type_t)type) {
    case MSG_READ_REG:   return "READ_REG";
    case MSG_WRITE_REG:  return "WRITE_REG";
    case MSG_READ_RESP:  return "READ_RESP";
    case MSG_WRITE_ACK:  return "WRITE_ACK";
    case MSG_ERROR:      return "ERROR";
    case MSG_PING:       return "PING";
    case MSG_PONG:       return "PONG";
    case MSG_RESET:      return "RESET";
    case MSG_RESET_ACK:  return "RESET_ACK";
    default:             return "UNKNOWN";
    }
}

const char *err_code_name(uint8_t code) {
    switch ((err_code_t)code) {
    case ERR_BAD_MAGIC:           return "BAD_MAGIC";
    case ERR_BAD_VERSION:         return "BAD_VERSION";
    case ERR_BAD_CRC:             return "BAD_CRC";
    case ERR_UNKNOWN_TYPE:        return "UNKNOWN_TYPE";
    case ERR_UNKNOWN_REGISTER:    return "UNKNOWN_REGISTER";
    case ERR_READ_ONLY_VIOLATION: return "READ_ONLY_VIOLATION";
    case ERR_INVALID_LENGTH:      return "INVALID_LENGTH";
    case ERR_INTERNAL:            return "INTERNAL";
    default:                      return "UNKNOWN_ERROR";
    }
}
