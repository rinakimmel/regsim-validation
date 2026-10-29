/*
 * crc32.c — CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
 *
 * Table-driven with lazy initialisation.  The table is 1 KiB and is built
 * at most once per process lifetime.
 */

#include "crc32.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal table
 * ---------------------------------------------------------------------- */

#define CRC32_POLY UINT32_C(0xEDB88320)   /* reflected IEEE 802.3 polynomial */

static uint32_t s_table[256];
static int      s_table_ready = 0;

/* -------------------------------------------------------------------------
 * Table initialisation
 * ---------------------------------------------------------------------- */

void crc32_init(void) {
    if (s_table_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ CRC32_POLY;
            } else {
                crc >>= 1;
            }
        }
        s_table[i] = crc;
    }
    s_table_ready = 1;
}

/* -------------------------------------------------------------------------
 * crc32_update
 * ---------------------------------------------------------------------- */

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    if (!s_table_ready) {
        crc32_init();
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t index = (uint8_t)((crc ^ data[i]) & 0xFFu);
        crc = (crc >> 8) ^ s_table[index];
    }
    return crc;
}

/* -------------------------------------------------------------------------
 * crc32_calc
 * ---------------------------------------------------------------------- */

uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, data, len);
    return crc ^ 0xFFFFFFFFu;
}
