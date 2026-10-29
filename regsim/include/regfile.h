/*
 * regfile.h — Register file model for the simulated chip.
 *
 * Register map (addresses are 16-bit):
 *
 *  Addr   Name          Access  Reset        Description
 *  0x0000 ID            RO      0xABCD1234   Chip identifier
 *  0x0002 VERSION       RO      0x00010000   Major.minor version
 *  0x0004 CTRL          RW      0x00000000   Control register
 *  0x0006 STATUS        RO/W1C  0x00000000   Status register
 *  0x0008 COUNTER       RO      0x00000000   Free-running access counter
 *  0x000A IRQ_MASK      RW      0x00000000   Interrupt mask
 *  0x000C IRQ_STATUS    W1C     0x00000000   Interrupt status (W1C bits)
 *  0x000E SCRATCH0      RW      0x00000000   Scratch register 0
 *  0x0010 SCRATCH1      RW      0x00000000   Scratch register 1
 *  0x0012 SCRATCH2      RW      0x00000000   Scratch register 2
 *  0x0014 SCRATCH3      RW      0x00000000   Scratch register 3
 *  0x0016 TIMER_RELOAD  RW      0x00000064   Timer reload value
 *  0x0018 TIMER_COUNT   RO      0x00000000   Current timer count
 *  0x001A CONFIG        RW      0x00000001   Configuration bits
 *  0x001C FIFO_DATA     WO      0x00000000   Write-only FIFO data port
 *  0x001E FIFO_STATUS   RO      0x00000001   FIFO flags (EMPTY on reset)
 *
 * CTRL bit fields:
 *   [0]    START  — writing 1 starts an operation (sets STATUS.BUSY)
 *   [1]    RESET  — writing 1 resets the state machine (self-clearing)
 *   [2]    IE     — interrupt enable
 *   [3]    MODE   — 0=normal, 1=loopback
 *   [31:4] RSVD   — reserved, reads as 0, writes ignored
 *
 * STATUS bit fields:
 *   [0]    BUSY   — RO, set when operation in progress
 *   [1]    DONE   — W1C, set when operation completes
 *   [2]    ERROR  — W1C, set on internal error
 *   [31:3] RSVD   — reserved
 *
 * IRQ_MASK / IRQ_STATUS share the same bit positions.
 *
 * CONFIG bit fields:
 *   [0]    ENABLE — global enable
 *   [1]    ENDIAN — 0=little, 1=big (cosmetic, does not change protocol)
 *   [31:2] RSVD   — reserved
 *
 * FIFO_STATUS bit fields:
 *   [0]    EMPTY  — 1 when FIFO is empty
 *   [1]    FULL   — 1 when FIFO is full
 *   [31:2] RSVD   — reserved
 */

#ifndef REGSIM_REGFILE_H
#define REGSIM_REGFILE_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Register addresses
 * ---------------------------------------------------------------------- */

#define REG_ID            UINT16_C(0x0000)
#define REG_VERSION       UINT16_C(0x0002)
#define REG_CTRL          UINT16_C(0x0004)
#define REG_STATUS        UINT16_C(0x0006)
#define REG_COUNTER       UINT16_C(0x0008)
#define REG_IRQ_MASK      UINT16_C(0x000A)
#define REG_IRQ_STATUS    UINT16_C(0x000C)
#define REG_SCRATCH0      UINT16_C(0x000E)
#define REG_SCRATCH1      UINT16_C(0x0010)
#define REG_SCRATCH2      UINT16_C(0x0012)
#define REG_SCRATCH3      UINT16_C(0x0014)
#define REG_TIMER_RELOAD  UINT16_C(0x0016)
#define REG_TIMER_COUNT   UINT16_C(0x0018)
#define REG_CONFIG        UINT16_C(0x001A)
#define REG_FIFO_DATA     UINT16_C(0x001C)
#define REG_FIFO_STATUS   UINT16_C(0x001E)

#define REGFILE_NUM_REGS  16u

/* -------------------------------------------------------------------------
 * CTRL bit masks
 * ---------------------------------------------------------------------- */

#define CTRL_START        UINT32_C(0x00000001)
#define CTRL_RESET_BIT    UINT32_C(0x00000002)
#define CTRL_IE           UINT32_C(0x00000004)
#define CTRL_MODE         UINT32_C(0x00000008)
#define CTRL_WRITABLE     UINT32_C(0x0000000F)  /* bits [3:0] are writable */

/* -------------------------------------------------------------------------
 * STATUS bit masks
 * ---------------------------------------------------------------------- */

#define STATUS_BUSY       UINT32_C(0x00000001)
#define STATUS_DONE       UINT32_C(0x00000002)  /* W1C */
#define STATUS_ERROR      UINT32_C(0x00000004)  /* W1C */
#define STATUS_W1C_MASK   UINT32_C(0x00000006)  /* DONE | ERROR */

/* -------------------------------------------------------------------------
 * IRQ bit masks
 * ---------------------------------------------------------------------- */

#define IRQ_DONE          UINT32_C(0x00000001)
#define IRQ_ERROR         UINT32_C(0x00000002)
#define IRQ_MASK_ALL      UINT32_C(0x00000003)

/* -------------------------------------------------------------------------
 * CONFIG bit masks
 * ---------------------------------------------------------------------- */

#define CONFIG_ENABLE     UINT32_C(0x00000001)
#define CONFIG_ENDIAN     UINT32_C(0x00000002)
#define CONFIG_WRITABLE   UINT32_C(0x00000003)

/* -------------------------------------------------------------------------
 * FIFO_STATUS bit masks
 * ---------------------------------------------------------------------- */

#define FIFO_ST_EMPTY     UINT32_C(0x00000001)
#define FIFO_ST_FULL      UINT32_C(0x00000002)

/* -------------------------------------------------------------------------
 * Reset values
 * ---------------------------------------------------------------------- */

#define RST_ID            UINT32_C(0xABCD1234)
#define RST_VERSION       UINT32_C(0x00010000)
#define RST_CTRL          UINT32_C(0x00000000)
#define RST_STATUS        UINT32_C(0x00000000)
#define RST_COUNTER       UINT32_C(0x00000000)
#define RST_IRQ_MASK      UINT32_C(0x00000000)
#define RST_IRQ_STATUS    UINT32_C(0x00000000)
#define RST_SCRATCH       UINT32_C(0x00000000)
#define RST_TIMER_RELOAD  UINT32_C(0x00000064)
#define RST_TIMER_COUNT   UINT32_C(0x00000000)
#define RST_CONFIG        UINT32_C(0x00000001)
#define RST_FIFO_DATA     UINT32_C(0x00000000)
#define RST_FIFO_STATUS   UINT32_C(0x00000001)  /* EMPTY=1 */

/* Number of register accesses before a started operation completes */
#define CTRL_BUSY_CLEAR_ACCESSES  4u

/* FIFO depth */
#define FIFO_DEPTH        16u

/* -------------------------------------------------------------------------
 * Access type enumeration
 * ---------------------------------------------------------------------- */

typedef enum {
    ACCESS_RW  = 0,   /* read-write */
    ACCESS_RO  = 1,   /* read-only */
    ACCESS_W1C = 2,   /* write-1-to-clear */
    ACCESS_WO  = 3,   /* write-only (reads return 0) */
} reg_access_t;

/* -------------------------------------------------------------------------
 * Register descriptor (static metadata)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint16_t     addr;
    const char  *name;
    uint32_t     reset_value;
    reg_access_t access;
    uint32_t     writable_mask;  /* bits that can be written */
    uint32_t     reserved_mask;  /* bits that always read as 0 */
} reg_desc_t;

/* -------------------------------------------------------------------------
 * Register file instance (mutable runtime state)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t values[REGFILE_NUM_REGS];  /* current register values */

    /* Busy countdown: when CTRL.START is written, this is set to
     * CTRL_BUSY_CLEAR_ACCESSES; it decrements on each register access and
     * clears BUSY / sets DONE when it reaches zero. */
    uint32_t busy_countdown;

    /* FIFO model */
    uint32_t fifo[FIFO_DEPTH];
    uint32_t fifo_head;
    uint32_t fifo_tail;
    uint32_t fifo_count;
} regfile_t;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/*
 * regfile_init — initialise *rf to power-on reset values.
 */
void regfile_init(regfile_t *rf);

/*
 * regfile_reset — soft-reset: identical to regfile_init.
 */
void regfile_reset(regfile_t *rf);

/*
 * regfile_read — read register at `addr` into *value.
 *
 * Returns  0 on success.
 * Returns -1 if addr is unknown.
 * The COUNTER register increments on every successful access (read or write).
 */
int regfile_read(regfile_t *rf, uint16_t addr, uint32_t *value);

/*
 * regfile_write — write `value` to register at `addr`.
 *
 * Returns  0 on success.
 * Returns -1 if addr is unknown.
 * Returns -2 if the register is read-only (ACCESS_RO).
 * The COUNTER register increments on every successful access.
 * W1C bits are cleared by writing 1; writing 0 has no effect on those bits.
 * Reserved bits in the written value are silently discarded.
 */
int regfile_write(regfile_t *rf, uint16_t addr, uint32_t value);

/*
 * regfile_tick — advance any time-based simulation (call after each
 * access).  Updates BUSY→DONE transition.
 */
void regfile_tick(regfile_t *rf);

/*
 * regfile_descriptor — return the static descriptor for register at `addr`,
 * or NULL if unknown.
 */
const reg_desc_t *regfile_descriptor(uint16_t addr);

/*
 * regfile_descriptor_by_index — return descriptor for the i-th register
 * (0 <= i < REGFILE_NUM_REGS).
 */
const reg_desc_t *regfile_descriptor_by_index(size_t i);

#endif /* REGSIM_REGFILE_H */
