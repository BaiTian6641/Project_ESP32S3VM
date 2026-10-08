/*
 * Edge-level behavioral tests for the real persistent JEDEC NOR service.
 * Link with esp32s3_spi_service.c as a QEMU tests/unit GLib executable.
 * No machine mock, source-text assertions, host time or synthetic RX map.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/ssi/esp32s3_spi_service.h"

typedef struct TestBus {
    S3SPINor chip;
    uint64_t now;
    bool mode3;
} TestBus;

static const S3SPINorConfig test_config = {
    .size_bytes = 131072,
    .jedec_id = { 0x37, 0x42, 0x17 },
    .program_ns = 100,
    .sector_erase_ns = 300,
    .block_erase_ns = 500,
    .chip_erase_ns = 700,
};

static void init_bus(TestBus *b, bool mode3)
{
    memset(b, 0, sizeof(*b));
    b->mode3 = mode3;
    b->now = 1000;
    g_assert_true(s3_spi_nor_init(&b->chip, &test_config));
}

/* Sample SO on actual rising edges. mode 0 ends each bit on falling;
 * mode 3 starts each bit on falling. Undriven levels are never read as data. */
static uint8_t transfer(TestBus *b, uint8_t out, bool expect_driven)
{
    uint8_t in = 0;
    int bit;
    bool miso, driven;

    for (bit = 7; bit >= 0; bit--) {
        bool mosi = (out >> bit) & 1;

        if (b->mode3) {
            s3_spi_nor_edge(&b->chip, false, mosi, b->now, &miso, &driven);
        }
        s3_spi_nor_edge(&b->chip, true, mosi, b->now, &miso, &driven);
        g_assert_cmpint(driven, ==, expect_driven);
        if (driven) {
            in = (in << 1) | miso;
        }
        if (!b->mode3) {
            s3_spi_nor_edge(&b->chip, false, mosi, b->now, &miso, &driven);
        }
    }
    return in;
}

static void select_bus(TestBus *b, bool active)
{
    s3_spi_nor_select(&b->chip, active, b->now);
}

static void command(TestBus *b, uint8_t opcode)
{
    select_bus(b, true);
    transfer(b, opcode, false);
    select_bus(b, false);
}

static uint8_t read_status(TestBus *b)
{
    uint8_t result;

    select_bus(b, true);
    transfer(b, 0x05, false);
    result = transfer(b, 0xa5, true);
    select_bus(b, false);
    return result;
}

static void address(TestBus *b, uint32_t addr)
{
    transfer(b, addr >> 16, false);
    transfer(b, addr >> 8, false);
    transfer(b, addr, false);
}

static void start_read(TestBus *b, uint8_t opcode, uint32_t addr)
{
    select_bus(b, true);
    transfer(b, opcode, false);
    address(b, addr);
    if (opcode == 0x0b) {
        transfer(b, 0x96, false);
    }
}

static uint8_t read_byte(TestBus *b, uint32_t addr)
{
    uint8_t result;

    start_read(b, 0x03, addr);
    result = transfer(b, 0xa5, true);
    select_bus(b, false);
    return result;
}

static void stage_program(TestBus *b, uint32_t addr,
                          const uint8_t *data, size_t size)
{
    size_t i;

    select_bus(b, true);
    transfer(b, 0x02, false);
    address(b, addr);
    for (i = 0; i < size; i++) {
        transfer(b, data[i], false);
    }
}

static void program(TestBus *b, uint32_t addr, uint8_t value)
{
    command(b, 0x06);
    g_assert_cmpuint(read_status(b), ==, 2);
    stage_program(b, addr, &value, 1);
    select_bus(b, false);
    g_assert_cmpuint(read_status(b), ==, 1);
    b->now += test_config.program_ns;
    g_assert_cmpuint(read_status(b), ==, 0);
    g_assert_cmpuint(read_byte(b, addr), ==, value);
}

static void test_config_and_ids(void)
{
    TestBus a, b;
    S3SPINor s;
    S3SPINorConfig c = test_config;
    unsigned int i;

    init_bus(&a, false);
    init_bus(&b, true);
    s3_spi_nor_cleanup(&b.chip);
    c.jedec_id[0] = 0xc2;
    g_assert_true(s3_spi_nor_init(&b.chip, &c));
    select_bus(&a, true);
    select_bus(&b, true);
    transfer(&a, 0x9f, false);
    transfer(&b, 0x9f, false);
    for (i = 0; i < 3; i++) {
        g_assert_cmpuint(transfer(&a, 0x00, true), ==, test_config.jedec_id[i]);
        g_assert_cmpuint(transfer(&b, 0xff, true), ==, c.jedec_id[i]);
    }
    g_assert_cmpuint(transfer(&a, 0x12, true), ==, 0xff);
    g_assert_cmpuint(transfer(&b, 0xed, true), ==, 0xff);
    select_bus(&a, false);
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&a), ==, 0);
    g_assert_cmpuint(read_status(&b), ==, 0);
    s3_spi_nor_cleanup(&a.chip);
    s3_spi_nor_cleanup(&b.chip);

    g_assert_true(s3_spi_nor_init(&s, NULL));
    g_assert_cmpuint(s.config.size_bytes, ==, 1024 * 1024);
    s3_spi_nor_cleanup(&s);
    c.size_bytes = 0;
    g_assert_false(s3_spi_nor_init(&s, &c));
    s3_spi_nor_cleanup(&s);
    c.size_bytes = 65535;
    g_assert_false(s3_spi_nor_init(&s, &c));
    s3_spi_nor_cleanup(&s);
    c.size_bytes = S3_SPI_NOR_MAX_SIZE + 1;
    g_assert_false(s3_spi_nor_init(&s, &c));
    s3_spi_nor_cleanup(&s);
    c = test_config;
    c.program_ns = 0;
    g_assert_false(s3_spi_nor_init(&s, &c));
    s3_spi_nor_cleanup(&s);
}

static void test_read_modes(void)
{
    unsigned int mode;

    for (mode = 0; mode < 2; mode++) {
        TestBus b;

        init_bus(&b, mode);
        g_assert_cmpuint(read_byte(&b, 0), ==, 0xff);
        g_assert_cmpuint(read_byte(&b, test_config.size_bytes - 1), ==, 0xff);
        program(&b, 0, 0x83);
        program(&b, 1, 0x52);
        program(&b, test_config.size_bytes - 1, 0xa6);
        start_read(&b, 0x03, test_config.size_bytes - 1);
        g_assert_cmpuint(transfer(&b, 0xff, true), ==, 0xa6);
        g_assert_cmpuint(transfer(&b, 0x00, true), ==, 0x83);
        g_assert_cmpuint(transfer(&b, 0x52, true), ==, 0x52);
        select_bus(&b, false);
        start_read(&b, 0x0b, 0x800000 + test_config.size_bytes);
        g_assert_cmpuint(transfer(&b, 0x12, true), ==, 0x83);
        g_assert_cmpuint(transfer(&b, 0x34, true), ==, 0x52);
        select_bus(&b, false);
        select_bus(&b, true);
        transfer(&b, 0xff, false);
        transfer(&b, 0x9f, false); /* Invalid command cannot restart under CS. */
        transfer(&b, 0x00, false);
        select_bus(&b, false);
        s3_spi_nor_cleanup(&b.chip);
    }
}

static void test_program_and_page_wrap(void)
{
    TestBus b;
    uint8_t values[] = { 0x12, 0x34, 0x56, 0x78 };
    uint8_t wrapped[257];
    size_t i;

    init_bus(&b, false);
    stage_program(&b, 0x2fe, values, sizeof(values)); /* No WEL. */
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 0);
    g_assert_cmpuint(read_byte(&b, 0x2fe), ==, 0xff);
    command(&b, 0x06);
    stage_program(&b, 0x2fe, values, sizeof(values));
    /* Duplicate assertion must not reset a partially accumulated command. */
    select_bus(&b, true);
    select_bus(&b, false);
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 1);
    b.now += test_config.program_ns - 1;
    g_assert_cmpuint(read_status(&b), ==, 1);
    b.now++;
    g_assert_cmpuint(read_status(&b), ==, 0);
    g_assert_cmpuint(read_byte(&b, 0x2fe), ==, 0x12);
    g_assert_cmpuint(read_byte(&b, 0x2ff), ==, 0x34);
    g_assert_cmpuint(read_byte(&b, 0x200), ==, 0x56);
    g_assert_cmpuint(read_byte(&b, 0x201), ==, 0x78);
    g_assert_cmpuint(read_byte(&b, 0x300), ==, 0xff);

    command(&b, 0x06);
    values[0] = 0x0f;
    stage_program(&b, 0x2fe, values, 1);
    select_bus(&b, false);
    b.now += test_config.program_ns;
    g_assert_cmpuint(read_byte(&b, 0x2fe), ==, 0x02);
    command(&b, 0x06);
    values[0] = 0xff;
    stage_program(&b, 0x2fe, values, 1);
    select_bus(&b, false);
    b.now += test_config.program_ns;
    g_assert_cmpuint(read_byte(&b, 0x2fe), ==, 0x02);

    memset(wrapped, 0xff, sizeof(wrapped));
    wrapped[0] = 0x00;
    wrapped[256] = 0x96;
    command(&b, 0x06);
    stage_program(&b, 0x410, wrapped, sizeof(wrapped));
    select_bus(&b, false);
    b.now += test_config.program_ns;
    /* Last latch byte wins, not an AND of repeated incoming bytes. */
    g_assert_cmpuint(read_byte(&b, 0x410), ==, 0x96);
    for (i = 0; i < 256; i++) {
        if (i != 0x10) {
            g_assert_cmpuint(read_byte(&b, 0x400 + i), ==, 0xff);
        }
    }
    s3_spi_nor_cleanup(&b.chip);
}

static void test_busy_and_streamed_status(void)
{
    static const uint8_t rejected[] = {
        0x9f, 0x03, 0x0b, 0x02, 0x20, 0xd8, 0xc7, 0x60, 0x06, 0x04, 0xff,
    };
    TestBus b;
    uint8_t value = 0x35;
    uint64_t deadline;
    size_t i;

    init_bus(&b, true);
    command(&b, 0x06);
    stage_program(&b, 0x1234, &value, 1);
    select_bus(&b, false);
    deadline = b.now + test_config.program_ns;
    for (i = 0; i < G_N_ELEMENTS(rejected); i++) {
        select_bus(&b, true);
        transfer(&b, rejected[i], false);
        transfer(&b, 0x00, false);
        transfer(&b, 0x00, false);
        transfer(&b, 0x00, false);
        transfer(&b, 0xff, false);
        select_bus(&b, false);
        g_assert_cmpuint(read_status(&b), ==, 1);
    }
    b.now = deadline - 1;
    select_bus(&b, true);
    transfer(&b, 0x03, false);
    b.now = deadline;
    /* An opcode rejected while busy is not revived by elapsed time. */
    transfer(&b, 0x00, false);
    transfer(&b, 0x12, false);
    transfer(&b, 0x34, false);
    transfer(&b, 0xff, false);
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 0);
    g_assert_cmpuint(read_byte(&b, 0x1234), ==, value);

    command(&b, 0x06);
    value = 0x12;
    stage_program(&b, 0x1235, &value, 1);
    select_bus(&b, false);
    deadline = b.now + test_config.program_ns;
    select_bus(&b, true);
    transfer(&b, 0x05, false);
    g_assert_cmpuint(transfer(&b, 0x00, true), ==, 1);
    b.now = deadline;
    /* A status byte already shifted into the output latch stays coherent.
     * The following byte reflects completion, without a new CS. */
    g_assert_cmpuint(transfer(&b, 0xff, true), ==, 1);
    g_assert_cmpuint(transfer(&b, 0xa5, true), ==, 0);
    select_bus(&b, false);
    g_assert_cmpuint(read_byte(&b, 0x1235), ==, 0x12);
    s3_spi_nor_cleanup(&b.chip);
}

static void partial_bits(TestBus *b, unsigned int count)
{
    bool miso, driven;
    unsigned int i;

    for (i = 0; i < count; i++) {
        if (b->mode3) {
            s3_spi_nor_edge(&b->chip, false, true, b->now, &miso, &driven);
        }
        s3_spi_nor_edge(&b->chip, true, true, b->now, &miso, &driven);
        if (!b->mode3) {
            s3_spi_nor_edge(&b->chip, false, true, b->now, &miso, &driven);
        }
    }
}

static void test_cs_boundaries(void)
{
    TestBus b;
    uint8_t value = 0x11;
    unsigned int bits;

    init_bus(&b, false);
    for (bits = 1; bits < 8; bits++) {
        select_bus(&b, true);
        partial_bits(&b, bits);
        select_bus(&b, false);
        g_assert_cmpuint(read_status(&b), ==, 0);
        select_bus(&b, true);
        transfer(&b, 0x06, false);
        partial_bits(&b, bits);
        select_bus(&b, false);
        g_assert_cmpuint(read_status(&b), ==, 0);
    }
    select_bus(&b, true);
    transfer(&b, 0x06, false);
    transfer(&b, 0x00, false);
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 0);
    command(&b, 0x06);
    command(&b, 0x04);
    g_assert_cmpuint(read_status(&b), ==, 0);
    command(&b, 0x06);
    stage_program(&b, 0x88, &value, 0); /* Address-only program. */
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 2);
    select_bus(&b, true);
    transfer(&b, 0x02, false);
    transfer(&b, 0x00, false);
    transfer(&b, 0x00, false);
    partial_bits(&b, 7); /* Interrupted address. */
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 2);
    for (bits = 1; bits < 8; bits++) {
        stage_program(&b, 0x88, &value, 1);
        partial_bits(&b, bits); /* Whole staged command must be discarded. */
        select_bus(&b, false);
        g_assert_cmpuint(read_status(&b), ==, 2);
        g_assert_cmpuint(read_byte(&b, 0x88), ==, 0xff);
    }
    stage_program(&b, 0x88, &value, 1);
    s3_spi_nor_abort(&b.chip, b.now);
    select_bus(&b, false); /* Must not commit the aborted complete bytes. */
    g_assert_cmpuint(read_status(&b), ==, 2);
    g_assert_cmpuint(read_byte(&b, 0x88), ==, 0xff);
    select_bus(&b, true);
    transfer(&b, 0x20, false);
    transfer(&b, 0x00, false);
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 2);
    select_bus(&b, true);
    transfer(&b, 0x20, false);
    address(&b, 0x88);
    transfer(&b, 0x00, false); /* Trailing erase data invalidates it. */
    select_bus(&b, false);
    g_assert_cmpuint(read_status(&b), ==, 2);
    stage_program(&b, 0x88, &value, 1);
    select_bus(&b, false);
    b.now += test_config.program_ns;
    g_assert_cmpuint(read_byte(&b, 0x88), ==, value);
    s3_spi_nor_cleanup(&b.chip);
}

static void erase(TestBus *b, uint8_t opcode, uint32_t addr, uint64_t duration)
{
    command(b, 0x06);
    select_bus(b, true);
    transfer(b, opcode, false);
    if (opcode == 0x20 || opcode == 0xd8) {
        address(b, addr);
    }
    select_bus(b, false);
    g_assert_cmpuint(read_status(b), ==, 1);
    b->now += duration - 1;
    g_assert_cmpuint(read_status(b), ==, 1);
    b->now++;
    g_assert_cmpuint(read_status(b), ==, 0);
}

static void test_erase_ranges(void)
{
    TestBus b;
    unsigned int alias;

    init_bus(&b, true);
    program(&b, 0x1000, 0x11);
    program(&b, 0x1fff, 0x22);
    program(&b, 0x0fff, 0x33);
    program(&b, 0x2000, 0x44);
    select_bus(&b, true);
    transfer(&b, 0x20, false); /* No WEL cannot erase. */
    address(&b, 0x1abc);
    select_bus(&b, false);
    g_assert_cmpuint(read_byte(&b, 0x1000), ==, 0x11);
    erase(&b, 0x20, 0x1abc, test_config.sector_erase_ns);
    g_assert_cmpuint(read_byte(&b, 0x1000), ==, 0xff);
    g_assert_cmpuint(read_byte(&b, 0x1fff), ==, 0xff);
    g_assert_cmpuint(read_byte(&b, 0x0fff), ==, 0x33);
    g_assert_cmpuint(read_byte(&b, 0x2000), ==, 0x44);
    program(&b, 0x10000, 0x55);
    program(&b, 0x1ffff, 0x66);
    erase(&b, 0xd8, 0x1cdef, test_config.block_erase_ns);
    g_assert_cmpuint(read_byte(&b, 0x10000), ==, 0xff);
    g_assert_cmpuint(read_byte(&b, 0x1ffff), ==, 0xff);
    g_assert_cmpuint(read_byte(&b, 0x0fff), ==, 0x33);
    g_assert_cmpuint(read_byte(&b, 0x2000), ==, 0x44);
    for (alias = 0; alias < 2; alias++) {
        program(&b, 0x10000, 0x77);
        erase(&b, alias ? 0x60 : 0xc7, 0, test_config.chip_erase_ns);
        g_assert_cmpuint(read_byte(&b, 0x0fff), ==, 0xff);
        g_assert_cmpuint(read_byte(&b, 0x2000), ==, 0xff);
        g_assert_cmpuint(read_byte(&b, 0x10000), ==, 0xff);
    }
    s3_spi_nor_cleanup(&b.chip);
}

static void test_power_and_controller_reset(void)
{
    TestBus b;
    uint8_t value = 0x22;
    bool miso, driven;
    uint64_t deadline;

    init_bus(&b, false);
    program(&b, 0x10, 0x11);
    command(&b, 0x06);
    stage_program(&b, 0x11, &value, 1);
    s3_spi_nor_power(&b.chip, false, b.now); /* CS power-loss abort. */
    select_bus(&b, false);
    select_bus(&b, true);
    s3_spi_nor_edge(&b.chip, false, true, b.now, &miso, &driven);
    g_assert_false(driven);
    s3_spi_nor_power(&b.chip, true, b.now);
    g_assert_cmpuint(read_status(&b), ==, 0);
    g_assert_cmpuint(read_byte(&b, 0x10), ==, 0x11);
    g_assert_cmpuint(read_byte(&b, 0x11), ==, 0xff);
    command(&b, 0x06);
    stage_program(&b, 0x11, &value, 1);
    select_bus(&b, false);
    deadline = b.now + test_config.program_ns;
    s3_spi_nor_abort(&b.chip, b.now);
    g_assert_cmpuint(read_status(&b), ==, 1);
    s3_spi_nor_power(&b.chip, true, b.now); /* Duplicate power keeps busy. */
    g_assert_cmpuint(read_status(&b), ==, 1);
    b.now = deadline - 1;
    s3_spi_nor_power(&b.chip, false, b.now);
    b.now = deadline + 100;
    s3_spi_nor_power(&b.chip, true, b.now);
    g_assert_cmpuint(read_status(&b), ==, 0);
    g_assert_cmpuint(read_byte(&b, 0x11), ==, 0xff);
    g_assert_cmpuint(read_byte(&b, 0x10), ==, 0x11);
    command(&b, 0x06);
    s3_spi_nor_abort(&b.chip, b.now);
    g_assert_cmpuint(read_status(&b), ==, 2); /* Controller reset keeps WEL. */
    stage_program(&b, 0x11, &value, 1);
    select_bus(&b, false);
    deadline = b.now + test_config.program_ns;
    b.now = deadline;
    s3_spi_nor_abort(&b.chip, b.now); /* Started operation survives reset. */
    g_assert_cmpuint(read_byte(&b, 0x11), ==, value);
    command(&b, 0x06);
    select_bus(&b, true);
    transfer(&b, 0xc7, false);
    select_bus(&b, false);
    b.now += test_config.chip_erase_ns - 1;
    s3_spi_nor_power(&b.chip, false, b.now); /* Pending erase is discarded. */
    b.now += 100;
    s3_spi_nor_power(&b.chip, true, b.now);
    g_assert_cmpuint(read_byte(&b, 0x10), ==, 0x11);
    g_assert_cmpuint(read_byte(&b, 0x11), ==, value);
    command(&b, 0x06);
    value = 0x44;
    stage_program(&b, 0x12, &value, 1);
    select_bus(&b, false);
    b.now += test_config.program_ns;
    s3_spi_nor_power(&b.chip, false, b.now); /* Deadline reached commits first. */
    s3_spi_nor_power(&b.chip, true, b.now);
    g_assert_cmpuint(read_byte(&b, 0x12), ==, value);
    s3_spi_nor_cleanup(&b.chip);
}

static void test_electrical_pipeline(void)
{
    TestBus b;
    bool miso, driven, before;

    init_bus(&b, true);
    s3_spi_nor_edge(&b.chip, false, true, b.now, &miso, &driven);
    g_assert_false(driven);
    select_bus(&b, true);
    transfer(&b, 0x9f, false);
    /* JEDEC 0x37 starts with zero, independently of MOSI high. */
    s3_spi_nor_edge(&b.chip, false, true, b.now, &miso, &driven);
    g_assert_true(driven);
    g_assert_false(miso);
    before = miso;
    s3_spi_nor_edge(&b.chip, true, true, b.now, &miso, &driven);
    g_assert_true(driven);
    g_assert_cmpint(miso, ==, before);
    /* Next zero then next one; rising never changes the physical SO line. */
    s3_spi_nor_edge(&b.chip, false, false, b.now, &miso, &driven);
    g_assert_false(miso);
    s3_spi_nor_edge(&b.chip, true, false, b.now, &miso, &driven);
    g_assert_false(miso);
    s3_spi_nor_edge(&b.chip, false, false, b.now, &miso, &driven);
    g_assert_true(miso);
    s3_spi_nor_edge(&b.chip, true, false, b.now, &miso, &driven);
    g_assert_true(miso);
    select_bus(&b, false);
    s3_spi_nor_edge(&b.chip, true, true, b.now, &miso, &driven);
    g_assert_false(driven);
    s3_spi_nor_cleanup(&b.chip);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/esp32s3/spi-nor/config-ids", test_config_and_ids);
    g_test_add_func("/esp32s3/spi-nor/read-modes", test_read_modes);
    g_test_add_func("/esp32s3/spi-nor/program-page-wrap", test_program_and_page_wrap);
    g_test_add_func("/esp32s3/spi-nor/busy-status", test_busy_and_streamed_status);
    g_test_add_func("/esp32s3/spi-nor/cs-boundaries", test_cs_boundaries);
    g_test_add_func("/esp32s3/spi-nor/erase-ranges", test_erase_ranges);
    g_test_add_func("/esp32s3/spi-nor/power-controller-reset",
                    test_power_and_controller_reset);
    g_test_add_func("/esp32s3/spi-nor/electrical-pipeline", test_electrical_pipeline);
    return g_test_run();
}
