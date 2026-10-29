/*
 * test_regfile.c — Unit tests for the register file model.
 *
 * Tests:
 *  - Reset values for all registers
 *  - RW read/write on all SCRATCH registers
 *  - Walking-ones pattern on SCRATCH registers
 *  - Walking-zeros pattern on SCRATCH registers
 *  - RO register write returns -2
 *  - W1C: writing 1 clears bits, writing 0 preserves bits
 *  - WO register reads as 0
 *  - Reserved bits read as 0 after write
 *  - COUNTER increments on every access
 *  - CTRL.START → STATUS.BUSY → STATUS.DONE sequence
 *  - CTRL.RESET clears STATUS
 *  - IRQ_MASK gates IRQ_STATUS assertion
 *  - Unknown register returns -1
 *  - RESET restores all registers to defaults
 *  - descriptor lookup by address and index
 */

#include "../include/test_framework.h"
#include "../include/regfile.h"
#include <string.h>

/* =========================================================================
 * Helpers
 * ====================================================================== */

/* Read a register and return its value; aborts test on error */
static uint32_t read_reg(test_suite_t *s, regfile_t *rf, uint16_t addr) {
    uint32_t v = 0;
    int rc = regfile_read(rf, addr, &v);
    TEST_ASSERT_EQ(s, rc, 0);
    return v;
}

static void write_reg(test_suite_t *s, regfile_t *rf,
                      uint16_t addr, uint32_t val) {
    int rc = regfile_write(rf, addr, val);
    TEST_ASSERT_EQ(s, rc, 0);
}

/* =========================================================================
 * Reset values
 * ====================================================================== */

static void test_reset_values(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* Spot-check every register reset value */
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_ID),          RST_ID);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_VERSION),     RST_VERSION);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_STATUS),      RST_STATUS);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_IRQ_MASK),    RST_IRQ_MASK);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_IRQ_STATUS),  RST_IRQ_STATUS);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_SCRATCH0),    RST_SCRATCH);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_SCRATCH1),    RST_SCRATCH);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_SCRATCH2),    RST_SCRATCH);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_SCRATCH3),    RST_SCRATCH);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_TIMER_RELOAD),RST_TIMER_RELOAD);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_CONFIG),      RST_CONFIG);
}

/* =========================================================================
 * RW read/write
 * ====================================================================== */

static void test_rw_scratch0(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    write_reg(s, &rf, REG_SCRATCH0, 0xDEADBEEFu);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_SCRATCH0), 0xDEADBEEFu);
}

static void test_rw_scratch_all(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    uint16_t addrs[] = { REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2, REG_SCRATCH3 };
    uint32_t vals[]  = { 0xAAAAAAAAu, 0x55555555u, 0x12345678u, 0xFEDCBA98u };
    for (int i = 0; i < 4; i++) {
        write_reg(s, &rf, addrs[i], vals[i]);
        TEST_ASSERT_HEX(s, read_reg(s, &rf, addrs[i]), vals[i]);
    }
}

/* =========================================================================
 * Walking-ones / walking-zeros
 * ====================================================================== */

static void test_walking_ones(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    for (int bit = 0; bit < 32; bit++) {
        uint32_t pattern = UINT32_C(1) << bit;
        write_reg(s, &rf, REG_SCRATCH0, pattern);
        uint32_t got = read_reg(s, &rf, REG_SCRATCH0);
        TEST_ASSERT_HEX(s, got, pattern);
    }
}

static void test_walking_zeros(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    for (int bit = 0; bit < 32; bit++) {
        uint32_t pattern = ~(UINT32_C(1) << bit);
        write_reg(s, &rf, REG_SCRATCH1, pattern);
        uint32_t got = read_reg(s, &rf, REG_SCRATCH1);
        TEST_ASSERT_HEX(s, got, pattern);
    }
}

/* =========================================================================
 * RO write rejection
 * ====================================================================== */

static void test_ro_write_rejected(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    /* ID is RO */
    int rc = regfile_write(&rf, REG_ID, 0x00000000u);
    TEST_ASSERT_EQ(s, rc, -2);
    /* Value must be unchanged */
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_ID), RST_ID);
}

static void test_ro_version_unchanged(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    (void)regfile_write(&rf, REG_VERSION, 0xFFFFFFFFu);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_VERSION), RST_VERSION);
}

/* =========================================================================
 * W1C behaviour
 * ====================================================================== */

static void test_w1c_clear_bit(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* Manually set STATUS bits by triggering CTRL.START and waiting */
    write_reg(s, &rf, REG_CTRL, CTRL_START);
    /* Burn through busy countdown */
    for (int i = 0; i < (int)CTRL_BUSY_CLEAR_ACCESSES + 2; i++) {
        uint32_t dummy;
        regfile_read(&rf, REG_SCRATCH0, &dummy);
    }
    uint32_t status = read_reg(s, &rf, REG_STATUS);
    TEST_ASSERT(s, (status & STATUS_DONE) != 0);

    /* Write 1 to DONE bit clears it */
    int rc = regfile_write(&rf, REG_STATUS, STATUS_DONE);
    TEST_ASSERT_EQ(s, rc, 0);

    status = read_reg(s, &rf, REG_STATUS);
    TEST_ASSERT(s, (status & STATUS_DONE) == 0);
}

static void test_w1c_zero_no_clear(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* Force STATUS.DONE set */
    write_reg(s, &rf, REG_CTRL, CTRL_START);
    for (int i = 0; i < (int)CTRL_BUSY_CLEAR_ACCESSES + 2; i++) {
        uint32_t dummy;
        regfile_read(&rf, REG_SCRATCH0, &dummy);
    }
    uint32_t before = read_reg(s, &rf, REG_STATUS);
    TEST_ASSERT(s, (before & STATUS_DONE) != 0);

    /* Writing 0 to W1C register must NOT clear anything */
    regfile_write(&rf, REG_STATUS, 0);
    uint32_t after = read_reg(s, &rf, REG_STATUS);
    TEST_ASSERT_HEX(s, after & STATUS_DONE, STATUS_DONE);
}

/* =========================================================================
 * WO reads as zero
 * ====================================================================== */

static void test_wo_reads_zero(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    /* Write something to FIFO_DATA (WO) */
    int rc = regfile_write(&rf, REG_FIFO_DATA, 0x12345678u);
    TEST_ASSERT_EQ(s, rc, 0);
    /* Read must return 0 */
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_FIFO_DATA), 0u);
}

/* =========================================================================
 * Reserved bits read as zero
 * ====================================================================== */

static void test_reserved_bits_zero(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    /* Write all-ones to CTRL — reserved bits [31:4] must not stick */
    write_reg(s, &rf, REG_CTRL, 0xFFFFFFFFu);
    uint32_t v = read_reg(s, &rf, REG_CTRL);
    /* Only CTRL_WRITABLE bits [3:0] should be set */
    TEST_ASSERT_HEX(s, v & ~CTRL_WRITABLE, 0u);
}

static void test_reserved_bits_config(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    write_reg(s, &rf, REG_CONFIG, 0xFFFFFFFFu);
    uint32_t v = read_reg(s, &rf, REG_CONFIG);
    TEST_ASSERT_HEX(s, v & ~CONFIG_WRITABLE, 0u);
}

/* =========================================================================
 * COUNTER monotonicity
 * ====================================================================== */

static void test_counter_increments(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* Read COUNTER itself multiple times — each read increments it */
    uint32_t c0 = read_reg(s, &rf, REG_COUNTER);
    uint32_t c1 = read_reg(s, &rf, REG_COUNTER);
    uint32_t c2 = read_reg(s, &rf, REG_COUNTER);

    TEST_ASSERT(s, c1 > c0);
    TEST_ASSERT(s, c2 > c1);
}

static void test_counter_on_write(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    uint32_t before = read_reg(s, &rf, REG_COUNTER);
    write_reg(s, &rf, REG_SCRATCH0, 0xABu);
    uint32_t after = read_reg(s, &rf, REG_COUNTER);
    TEST_ASSERT(s, after > before);
}

/* =========================================================================
 * CTRL/STATUS busy→done sequence
 * ====================================================================== */

static void test_ctrl_start_busy_done(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* Before START: BUSY must be clear */
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_STATUS) & STATUS_BUSY, 0u);

    /* Write CTRL.START */
    write_reg(s, &rf, REG_CTRL, CTRL_START);

    /* Immediately after: BUSY must be set */
    uint32_t st = read_reg(s, &rf, REG_STATUS);
    TEST_ASSERT(s, (st & STATUS_BUSY) != 0);
    TEST_ASSERT(s, (st & STATUS_DONE) == 0);

    /* Drain the busy countdown by doing accesses */
    for (int i = 0; i < (int)CTRL_BUSY_CLEAR_ACCESSES + 2; i++) {
        uint32_t dummy;
        regfile_read(&rf, REG_SCRATCH0, &dummy);
    }

    /* Now BUSY should be clear, DONE set */
    st = read_reg(s, &rf, REG_STATUS);
    TEST_ASSERT(s, (st & STATUS_BUSY) == 0);
    TEST_ASSERT(s, (st & STATUS_DONE) != 0);
}

static void test_ctrl_reset_clears_status(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* Start an operation */
    write_reg(s, &rf, REG_CTRL, CTRL_START);

    /* Issue CTRL.RESET */
    write_reg(s, &rf, REG_CTRL, CTRL_RESET_BIT);

    /* STATUS should be cleared */
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_STATUS), 0u);

    /* CTRL.RESET bit should self-clear */
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_CTRL) & CTRL_RESET_BIT, 0u);
}

/* =========================================================================
 * IRQ masking
 * ====================================================================== */

static void test_irq_masked_off(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* IRQ_MASK = 0: no IRQ asserted after operation completes */
    write_reg(s, &rf, REG_IRQ_MASK, 0);
    write_reg(s, &rf, REG_CTRL, CTRL_START);
    for (int i = 0; i < (int)CTRL_BUSY_CLEAR_ACCESSES + 2; i++) {
        uint32_t d; regfile_read(&rf, REG_SCRATCH0, &d);
    }
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_IRQ_STATUS), 0u);
}

static void test_irq_masked_on(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* IRQ_MASK.DONE = 1 → IRQ_STATUS.DONE set after operation */
    write_reg(s, &rf, REG_IRQ_MASK, IRQ_DONE);
    write_reg(s, &rf, REG_CTRL, CTRL_START);
    for (int i = 0; i < (int)CTRL_BUSY_CLEAR_ACCESSES + 2; i++) {
        uint32_t d; regfile_read(&rf, REG_SCRATCH0, &d);
    }
    TEST_ASSERT(s, (read_reg(s, &rf, REG_IRQ_STATUS) & IRQ_DONE) != 0);
}

/* =========================================================================
 * Unknown register
 * ====================================================================== */

static void test_unknown_register_read(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    uint32_t v;
    int rc = regfile_read(&rf, 0xFFFFu, &v);
    TEST_ASSERT_EQ(s, rc, -1);
}

static void test_unknown_register_write(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);
    int rc = regfile_write(&rf, 0xDEADu, 0);
    TEST_ASSERT_EQ(s, rc, -1);
}

/* =========================================================================
 * Full reset restores defaults
 * ====================================================================== */

static void test_reset_restores_defaults(test_suite_t *s) {
    regfile_t rf;
    regfile_init(&rf);

    /* Dirty several registers */
    write_reg(s, &rf, REG_SCRATCH0,    0xDEADBEEFu);
    write_reg(s, &rf, REG_SCRATCH1,    0x12345678u);
    write_reg(s, &rf, REG_IRQ_MASK,    IRQ_MASK_ALL);
    write_reg(s, &rf, REG_TIMER_RELOAD, 0x1234u);
    write_reg(s, &rf, REG_CTRL,        CTRL_START);

    /* Issue software reset */
    regfile_reset(&rf);

    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_SCRATCH0),     RST_SCRATCH);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_SCRATCH1),     RST_SCRATCH);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_IRQ_MASK),     RST_IRQ_MASK);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_TIMER_RELOAD), RST_TIMER_RELOAD);
    TEST_ASSERT_HEX(s, read_reg(s, &rf, REG_STATUS),       RST_STATUS);
}

/* =========================================================================
 * Descriptor lookup
 * ====================================================================== */

static void test_descriptor_by_addr(test_suite_t *s) {
    const reg_desc_t *d = regfile_descriptor(REG_ID);
    TEST_ASSERT(s, d != NULL);
    if (!d) return;
    TEST_ASSERT_EQ(s, d->addr,   REG_ID);
    TEST_ASSERT_EQ(s, d->access, ACCESS_RO);
    TEST_ASSERT_HEX(s, d->reset_value, RST_ID);
}

static void test_descriptor_unknown(test_suite_t *s) {
    const reg_desc_t *d = regfile_descriptor(0x9999u);
    TEST_ASSERT(s, d == NULL);
}

static void test_descriptor_by_index(test_suite_t *s) {
    /* All 16 descriptors must be non-NULL */
    for (size_t i = 0; i < REGFILE_NUM_REGS; i++) {
        const reg_desc_t *d = regfile_descriptor_by_index(i);
        TEST_ASSERT(s, d != NULL);
    }
    /* Index out of range must return NULL */
    TEST_ASSERT(s, regfile_descriptor_by_index(REGFILE_NUM_REGS) == NULL);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    test_suite_t *s = test_suite_new("Register File Unit Tests");

    RUN_TEST(s, test_reset_values);
    RUN_TEST(s, test_rw_scratch0);
    RUN_TEST(s, test_rw_scratch_all);
    RUN_TEST(s, test_walking_ones);
    RUN_TEST(s, test_walking_zeros);
    RUN_TEST(s, test_ro_write_rejected);
    RUN_TEST(s, test_ro_version_unchanged);
    RUN_TEST(s, test_w1c_clear_bit);
    RUN_TEST(s, test_w1c_zero_no_clear);
    RUN_TEST(s, test_wo_reads_zero);
    RUN_TEST(s, test_reserved_bits_zero);
    RUN_TEST(s, test_reserved_bits_config);
    RUN_TEST(s, test_counter_increments);
    RUN_TEST(s, test_counter_on_write);
    RUN_TEST(s, test_ctrl_start_busy_done);
    RUN_TEST(s, test_ctrl_reset_clears_status);
    RUN_TEST(s, test_irq_masked_off);
    RUN_TEST(s, test_irq_masked_on);
    RUN_TEST(s, test_unknown_register_read);
    RUN_TEST(s, test_unknown_register_write);
    RUN_TEST(s, test_reset_restores_defaults);
    RUN_TEST(s, test_descriptor_by_addr);
    RUN_TEST(s, test_descriptor_unknown);
    RUN_TEST(s, test_descriptor_by_index);

    return test_suite_finish(s);
}
