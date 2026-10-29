/*
 * regfile.c — Simulated chip register file implementation.
 *
 * Implements access semantics (RW, RO, W1C, WO), reserved-bit masking,
 * CTRL/STATUS busy-done simulation, free-running COUNTER, and FIFO model.
 */

#include "regfile.h"
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Static register descriptor table
 * ---------------------------------------------------------------------- */

static const reg_desc_t s_regs[REGFILE_NUM_REGS] = {
    /* addr            name            reset              access       writable_mask       reserved_mask */
    { REG_ID,          "ID",           RST_ID,            ACCESS_RO,   0x00000000u,        0x00000000u },
    { REG_VERSION,     "VERSION",      RST_VERSION,       ACCESS_RO,   0x00000000u,        0x00000000u },
    { REG_CTRL,        "CTRL",         RST_CTRL,          ACCESS_RW,   CTRL_WRITABLE,      ~CTRL_WRITABLE },
    { REG_STATUS,      "STATUS",       RST_STATUS,        ACCESS_W1C,  STATUS_W1C_MASK,    ~(STATUS_BUSY | STATUS_DONE | STATUS_ERROR) },
    { REG_COUNTER,     "COUNTER",      RST_COUNTER,       ACCESS_RO,   0x00000000u,        0x00000000u },
    { REG_IRQ_MASK,    "IRQ_MASK",     RST_IRQ_MASK,      ACCESS_RW,   IRQ_MASK_ALL,       ~IRQ_MASK_ALL },
    { REG_IRQ_STATUS,  "IRQ_STATUS",   RST_IRQ_STATUS,    ACCESS_W1C,  IRQ_MASK_ALL,       ~IRQ_MASK_ALL },
    { REG_SCRATCH0,    "SCRATCH0",     RST_SCRATCH,       ACCESS_RW,   0xFFFFFFFFu,        0x00000000u },
    { REG_SCRATCH1,    "SCRATCH1",     RST_SCRATCH,       ACCESS_RW,   0xFFFFFFFFu,        0x00000000u },
    { REG_SCRATCH2,    "SCRATCH2",     RST_SCRATCH,       ACCESS_RW,   0xFFFFFFFFu,        0x00000000u },
    { REG_SCRATCH3,    "SCRATCH3",     RST_SCRATCH,       ACCESS_RW,   0xFFFFFFFFu,        0x00000000u },
    { REG_TIMER_RELOAD,"TIMER_RELOAD", RST_TIMER_RELOAD,  ACCESS_RW,   0xFFFFFFFFu,        0x00000000u },
    { REG_TIMER_COUNT, "TIMER_COUNT",  RST_TIMER_COUNT,   ACCESS_RO,   0x00000000u,        0x00000000u },
    { REG_CONFIG,      "CONFIG",       RST_CONFIG,        ACCESS_RW,   CONFIG_WRITABLE,    ~CONFIG_WRITABLE },
    { REG_FIFO_DATA,   "FIFO_DATA",    RST_FIFO_DATA,     ACCESS_WO,   0xFFFFFFFFu,        0x00000000u },
    { REG_FIFO_STATUS, "FIFO_STATUS",  RST_FIFO_STATUS,   ACCESS_RO,   0x00000000u,        ~(FIFO_ST_EMPTY | FIFO_ST_FULL) },
};

/* -------------------------------------------------------------------------
 * Index lookup
 * ---------------------------------------------------------------------- */

static int reg_index(uint16_t addr) {
    for (int i = 0; i < (int)REGFILE_NUM_REGS; i++) {
        if (s_regs[i].addr == addr) return i;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * regfile_init / regfile_reset
 * ---------------------------------------------------------------------- */

void regfile_init(regfile_t *rf) {
    memset(rf, 0, sizeof(*rf));
    for (size_t i = 0; i < REGFILE_NUM_REGS; i++) {
        rf->values[i] = s_regs[i].reset_value;
    }
    /* FIFO starts empty */
    rf->fifo_head  = 0;
    rf->fifo_tail  = 0;
    rf->fifo_count = 0;
    /* Update FIFO_STATUS register to reflect empty state */
    int fi = reg_index(REG_FIFO_STATUS);
    if (fi >= 0) rf->values[fi] = FIFO_ST_EMPTY;
}

void regfile_reset(regfile_t *rf) {
    regfile_init(rf);
}

/* -------------------------------------------------------------------------
 * Internal FIFO helpers
 * ---------------------------------------------------------------------- */

static void _fifo_push(regfile_t *rf, uint32_t value) {
    if (rf->fifo_count >= FIFO_DEPTH) {
        /* FIFO full — drop the oldest entry (overwrite) */
        rf->fifo[rf->fifo_tail] = value;
        rf->fifo_head = (rf->fifo_head + 1u) % FIFO_DEPTH;
    } else {
        rf->fifo[rf->fifo_tail] = value;
        rf->fifo_count++;
    }
    rf->fifo_tail = (rf->fifo_tail + 1u) % FIFO_DEPTH;
}

static void _update_fifo_status(regfile_t *rf) {
    int fi = reg_index(REG_FIFO_STATUS);
    if (fi < 0) return;
    uint32_t st = 0;
    if (rf->fifo_count == 0)         st |= FIFO_ST_EMPTY;
    if (rf->fifo_count >= FIFO_DEPTH) st |= FIFO_ST_FULL;
    rf->values[fi] = st;
}

/* -------------------------------------------------------------------------
 * regfile_read
 * ---------------------------------------------------------------------- */

int regfile_read(regfile_t *rf, uint16_t addr, uint32_t *value) {
    int idx = reg_index(addr);
    if (idx < 0) return -1;

    const reg_desc_t *d = &s_regs[idx];

    /* WO registers always read as 0 */
    if (d->access == ACCESS_WO) {
        *value = 0u;
    } else {
        /* Mask out reserved bits */
        *value = rf->values[idx] & ~d->reserved_mask;
    }

    /* Increment the free-running counter on every successful access */
    int ci = reg_index(REG_COUNTER);
    if (ci >= 0 && addr != REG_COUNTER) {
        rf->values[ci]++;
    } else if (ci >= 0 && addr == REG_COUNTER) {
        /* Reading COUNTER also increments it (self-increment) */
        *value = rf->values[ci];
        rf->values[ci]++;
    }

    regfile_tick(rf);
    return 0;
}

/* -------------------------------------------------------------------------
 * regfile_write
 * ---------------------------------------------------------------------- */

int regfile_write(regfile_t *rf, uint16_t addr, uint32_t value) {
    int idx = reg_index(addr);
    if (idx < 0) return -1;

    const reg_desc_t *d = &s_regs[idx];

    if (d->access == ACCESS_RO) {
        return -2;  /* read-only violation */
    }

    /* Mask away reserved and non-writable bits */
    uint32_t masked = value & d->writable_mask;

    switch (d->access) {
    case ACCESS_RW:
        rf->values[idx] = masked;
        break;

    case ACCESS_W1C:
        /* Writing 1 clears the bit; writing 0 leaves it unchanged */
        rf->values[idx] &= ~masked;
        break;

    case ACCESS_WO:
        /* Write-only: store to FIFO if this is FIFO_DATA */
        if (addr == REG_FIFO_DATA) {
            _fifo_push(rf, value);
            _update_fifo_status(rf);
        }
        /* Value is not reflected in a readable register */
        break;

    case ACCESS_RO:
        return -2;
    }

    /* ---- Special behaviour for CTRL register ---- */
    if (addr == REG_CTRL) {
        /* CTRL.RESET: self-clearing bit that resets the state machine */
        if (masked & CTRL_RESET_BIT) {
            /* Clear the reset bit */
            rf->values[idx] &= ~CTRL_RESET_BIT;
            /* Clear STATUS */
            int si = reg_index(REG_STATUS);
            if (si >= 0) rf->values[si] = 0;
            rf->busy_countdown = 0;
        }
        /* CTRL.START: sets STATUS.BUSY and begins countdown */
        if (masked & CTRL_START) {
            int si = reg_index(REG_STATUS);
            if (si >= 0) {
                rf->values[si] |= STATUS_BUSY;
                rf->values[si] &= ~(STATUS_DONE | STATUS_ERROR);
            }
            rf->busy_countdown = CTRL_BUSY_CLEAR_ACCESSES;
        }
    }

    /* Increment the free-running counter on every successful access */
    int ci = reg_index(REG_COUNTER);
    if (ci >= 0 && addr != REG_COUNTER) {
        rf->values[ci]++;
    }

    regfile_tick(rf);
    return 0;
}

/* -------------------------------------------------------------------------
 * regfile_tick
 * ---------------------------------------------------------------------- */

void regfile_tick(regfile_t *rf) {
    if (rf->busy_countdown == 0) return;

    rf->busy_countdown--;
    if (rf->busy_countdown == 0) {
        int si = reg_index(REG_STATUS);
        if (si >= 0) {
            rf->values[si] &= ~STATUS_BUSY;
            rf->values[si] |=  STATUS_DONE;
        }
        /* If IRQ_MASK.DONE is set, assert IRQ_STATUS.DONE */
        int mi  = reg_index(REG_IRQ_MASK);
        int isi = reg_index(REG_IRQ_STATUS);
        if (mi >= 0 && isi >= 0) {
            if (rf->values[mi] & IRQ_DONE) {
                rf->values[isi] |= IRQ_DONE;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Descriptor accessors
 * ---------------------------------------------------------------------- */

const reg_desc_t *regfile_descriptor(uint16_t addr) {
    int idx = reg_index(addr);
    if (idx < 0) return NULL;
    return &s_regs[idx];
}

const reg_desc_t *regfile_descriptor_by_index(size_t i) {
    if (i >= REGFILE_NUM_REGS) return NULL;
    return &s_regs[i];
}
