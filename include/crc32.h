/*
 * crc32.h — CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
 *
 * Table-driven, lazily initialised on first use.
 * Thread safety: the table is written exactly once before any read (via
 * crc32_init); call crc32_init() explicitly from main() if you care about
 * multi-threaded use before the first crc32_update() call.
 */

#ifndef REGSIM_CRC32_H
#define REGSIM_CRC32_H

#include <stdint.h>
#include <stddef.h>

/* Pre-build the lookup table.  Safe to call multiple times. */
void crc32_init(void);

/*
 * crc32_update — extend a running CRC over `len` bytes starting at `data`.
 *
 * To start a new computation pass 0xFFFFFFFF as `crc`.
 * After the last chunk, XOR the result with 0xFFFFFFFF to get the final CRC.
 *
 * Example:
 *   uint32_t crc = 0xFFFFFFFF;
 *   crc = crc32_update(crc, buf, len);
 *   crc ^= 0xFFFFFFFF;   // final CRC
 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/*
 * crc32_calc — convenience wrapper: compute CRC over a single contiguous
 * buffer and return the final value.
 */
uint32_t crc32_calc(const uint8_t *data, size_t len);

#endif /* REGSIM_CRC32_H */
