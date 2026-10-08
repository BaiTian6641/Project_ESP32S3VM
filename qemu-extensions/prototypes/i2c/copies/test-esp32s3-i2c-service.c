/*
 * Standalone GLib unit tests, suitable for a QEMU tests/unit executable linked
 * with esp32s3_i2c_service.c; no machine, firmware, transport or wall time.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/i2c/esp32s3_i2c_service.h"

#define MS INT64_C(1000000)

/* Independent polynomial long division, rather than the service's CRC loop. */
static uint8_t frame_crc(const uint8_t frame[3])
{
    uint32_t remainder = ((uint32_t)frame[0] << 16) |
                         ((uint32_t)frame[1] << 8);
    int bit;

    for (bit = 23; bit >= 8; bit--) {
        if (remainder & (1u << bit)) {
            remainder ^= 0x131u << (bit - 8);
        }
    }
    return remainder;
}

static void read_frame(S3I2CService *s, int64_t now, uint8_t frame[3])
{
    unsigned int i;
    uint8_t sentinel = 0xa5;

    for (i = 0; i < 3; i++) {
        g_assert_true(s3_i2c_service_read(s, &frame[i], now));
        s3_i2c_service_read_ack(s, i == 2);
    }
    g_assert_cmpuint(frame[2], ==, frame_crc(frame));
    g_assert_true(s->final_nack);
    g_assert_false(s3_i2c_service_read(s, &sentinel, now));
    g_assert_cmpuint(sentinel, ==, 0xa5);
}

static void write_user(S3I2CService *s, uint8_t value, int64_t now)
{
    g_assert_true(s3_i2c_service_address(s, false, false, now));
    g_assert_true(s3_i2c_service_write(s, 0xe6, now));
    g_assert_true(s3_i2c_service_write(s, value, now));
    g_assert_false(s3_i2c_service_write(s, value, now));
    s3_i2c_service_stop(s, now);
}

static uint8_t read_user(S3I2CService *s, int64_t now)
{
    uint8_t value;
    uint8_t sentinel = 0x55;

    g_assert_true(s3_i2c_service_address(s, false, false, now));
    g_assert_true(s3_i2c_service_write(s, 0xe7, now));
    g_assert_true(s3_i2c_service_address(s, true, true, now));
    g_assert_true(s3_i2c_service_read(s, &value, now));
    /* Only one register byte, even if the master erroneously ACKs it. */
    s3_i2c_service_read_ack(s, false);
    g_assert_false(s3_i2c_service_read(s, &sentinel, now));
    g_assert_cmpuint(sentinel, ==, 0x55);
    s3_i2c_service_read_ack(s, true);
    s3_i2c_service_stop(s, now);
    return value;
}

static int64_t measure(S3I2CService *s, uint8_t command, int64_t now,
                       uint8_t frame[3])
{
    int64_t ready;

    g_assert_true(s3_i2c_service_address(s, false, false, now));
    g_assert_true(s3_i2c_service_write(s, command, now));
    ready = s3_i2c_service_ready_ns(s);
    s3_i2c_service_stop(s, now);
    g_assert_true(s3_i2c_service_address(s, true, false, ready));
    read_frame(s, ready, frame);
    s3_i2c_service_stop(s, ready);
    return ready;
}

static void test_sht_no_hold(void)
{
    const uint8_t commands[] = { 0xf3, 0xf5 };
    const int64_t durations[] = { 85 * MS, 29 * MS };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(commands); i++) {
        S3I2CService s;
        uint8_t frame[3], next[3];
        int64_t now = 123456;
        int64_t ready = now + durations[i];

        s3_i2c_service_init(&s, S3_I2C_SHT21);
        g_assert_true(s3_i2c_service_address(&s, false, false, now));
        g_assert_true(s3_i2c_service_write(&s, commands[i], now));
        g_assert_false(s3_i2c_service_write(&s, 0, now));
        g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, ready);
        s3_i2c_service_stop(&s, now);
        g_assert_true(s.conversion_pending);
        g_assert_false(s3_i2c_service_address(&s, true, false, now));
        g_assert_false(s3_i2c_service_address(&s, true, false, ready - 1));
        g_assert_true(s3_i2c_service_address(&s, true, false, ready));
        read_frame(&s, ready, frame);
        g_assert_cmpuint(frame[1] & 3, ==, i ? 2 : 0);
        g_assert_false(s.conversion_pending);
        s3_i2c_service_stop(&s, ready);
        g_assert_true(s.final_nack);
        /* New address clears final NACK and re-reads the retained result. */
        g_assert_true(s3_i2c_service_address(&s, true, false, ready));
        read_frame(&s, ready, next);
        g_assert_cmpmem(frame, sizeof(frame), next, sizeof(next));
        s3_i2c_service_stop(&s, ready);
        measure(&s, commands[i], ready, next);
        g_assert_cmpuint((frame[0] << 8) | frame[1], !=,
                         (next[0] << 8) | next[1]);
        g_assert_cmpuint(s.sample_number, ==, 2);
    }
}

static void test_sht_final_nack_and_frame_end(void)
{
    S3I2CService s;
    uint8_t frame[3];
    uint8_t byte = 0xa5;
    unsigned int i;

    s3_i2c_service_init(&s, S3_I2C_SHT21);
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0xf5, 0));
    s3_i2c_service_stop(&s, 0);
    g_assert_true(s3_i2c_service_address(&s, true, false, 29 * MS));
    for (i = 0; i < 2; i++) {
        g_assert_true(s3_i2c_service_read(&s, &frame[i], 29 * MS));
        s3_i2c_service_read_ack(&s, i == 1);
    }
    /* NACK on the data LSB explicitly omits an otherwise available CRC. */
    g_assert_true(s.final_nack);
    g_assert_cmpuint(s.response_pos, ==, 2);
    s3_i2c_service_read_ack(&s, false);
    g_assert_false(s3_i2c_service_read(&s, &byte, 29 * MS));
    g_assert_cmpuint(byte, ==, 0xa5);
    g_assert_cmpuint(s.response_pos, ==, 2);
    g_assert_true(s3_i2c_service_address(&s, true, true, 29 * MS));
    g_assert_false(s.final_nack);
    for (i = 0; i < 3; i++) {
        g_assert_true(s3_i2c_service_read(&s, &frame[i], 29 * MS));
        s3_i2c_service_read_ack(&s, false);
    }
    g_assert_cmpuint(frame[2], ==, frame_crc(frame));
    /* Frame exhaustion must also fail when no final NACK was issued. */
    g_assert_false(s.final_nack);
    g_assert_false(s3_i2c_service_read(&s, &byte, 29 * MS));
    g_assert_cmpuint(byte, ==, 0xa5);
}

static void test_sht_hold_cancel(void)
{
    const uint8_t commands[] = { 0xe3, 0xe5 };
    const int64_t durations[] = { 85 * MS, 29 * MS };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(commands); i++) {
        S3I2CService s;
        uint8_t byte = 0xa5;
        uint8_t frame[3];

        s3_i2c_service_init(&s, S3_I2C_SHT21);
        g_assert_true(s3_i2c_service_address(&s, false, false, 0));
        g_assert_true(s3_i2c_service_write(&s, commands[i], 0));
        g_assert_true(s3_i2c_service_address(&s, true, true, 0));
        g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, durations[i]);
        g_assert_false(s3_i2c_service_read(&s, &byte, durations[i] - 1));
        g_assert_cmpuint(byte, ==, 0xa5);
        g_assert_cmpuint(s.response_pos, ==, 0);
        s3_i2c_service_cancel(&s);
        g_assert_true(s.conversion_pending);
        g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, durations[i]);
        g_assert_false(s3_i2c_service_read(&s, &byte, durations[i]));
        g_assert_true(s3_i2c_service_address(&s, true, false, durations[i]));
        read_frame(&s, durations[i], frame);
        g_assert_cmpuint(frame[1] & 3, ==, i ? 2 : 0);
    }
}

static void test_sht_resolution(void)
{
    const uint8_t settings[] = { 0x3a, 0x3b, 0xba, 0xbb };
    const unsigned int t_ms[] = { 85, 22, 43, 11 };
    const unsigned int h_ms[] = { 29, 4, 9, 15 };
    const unsigned int t_unused[] = { 2, 4, 3, 5 };
    const unsigned int h_unused[] = { 4, 8, 6, 5 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(settings); i++) {
        S3I2CService s;
        uint8_t frame[3];
        uint16_t raw;
        int64_t now;

        s3_i2c_service_init(&s, S3_I2C_SHT21);
        write_user(&s, settings[i], 0);
        g_assert_cmpuint(read_user(&s, 0), ==, settings[i]);
        now = measure(&s, 0xf3, 0, frame);
        g_assert_cmpint(now, ==, t_ms[i] * MS);
        raw = (frame[0] << 8) | frame[1];
        g_assert_cmpuint(raw & ((1u << t_unused[i]) - 1), ==, 0);
        now = measure(&s, 0xf5, now, frame) - now;
        g_assert_cmpint(now, ==, h_ms[i] * MS);
        raw = (frame[0] << 8) | frame[1];
        g_assert_cmpuint(raw & ((1u << h_unused[i]) - 1), ==, 2);
    }
}

static void test_sht_otp_reset(void)
{
    S3I2CService s;
    uint8_t initial[3], after_reset[3];
    int64_t ready;

    s3_i2c_service_init(&s, S3_I2C_SHT21);
    /* Heater on, low resolution, OTP reload enabled. */
    write_user(&s, 0xbd, 0);
    ready = measure(&s, 0xf3, 0, initial);
    g_assert_cmpint(ready, ==, 85 * MS + 2500000);
    g_assert_cmpuint(read_user(&s, ready), ==, 0x3e);
    write_user(&s, 0xbf, ready);
    g_assert_true(s3_i2c_service_address(&s, false, false, ready));
    g_assert_true(s3_i2c_service_write(&s, 0xf5, ready));
    s3_i2c_service_stop(&s, ready);
    g_assert_true(s3_i2c_service_address(&s, false, false, ready + 1));
    /* Reset interrupts the pending humidity conversion, unlike STOP. */
    g_assert_true(s3_i2c_service_write(&s, 0xfe, ready + 1));
    g_assert_false(s.conversion_pending);
    g_assert_cmpuint(s.sample_number, ==, 0);
    g_assert_cmpuint(s.user_register, ==, 0x3e);
    s3_i2c_service_stop(&s, ready + 1);
    ready += 1 + 15 * MS;
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, ready);
    g_assert_false(s3_i2c_service_address(&s, false, false, ready - 1));
    g_assert_false(s3_i2c_service_address(&s, true, false, ready - 1));
    /* Reset completion itself is not a readable measurement. */
    g_assert_false(s3_i2c_service_address(&s, true, false, ready));
    g_assert_cmpuint(read_user(&s, ready), ==, 0x3e);
    measure(&s, 0xf3, ready, after_reset);
    g_assert_cmpmem(initial, sizeof(initial), after_reset, sizeof(after_reset));
    s3_i2c_service_init(&s, S3_I2C_SHT21);
    g_assert_cmpuint(read_user(&s, 0), ==, 0x3a);
    g_assert_cmpuint(s.sample_number, ==, 0);
}

static void test_sht_invalid_writes(void)
{
    S3I2CService s;
    uint8_t byte = 0xa5;
    const uint8_t invalid[] = { 0x00, 0x7a, 0x32, 0xff };
    unsigned int i;

    s3_i2c_service_init(&s, S3_I2C_SHT21);
    g_assert_false(s3_i2c_service_read(&s, &byte, 0));
    g_assert_false(s3_i2c_service_write(&s, 0xf3, 0));
    g_assert_false(s3_i2c_service_address(&s, true, false, 0));
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_false(s3_i2c_service_write(&s, 0x99, 0));
    g_assert_false(s3_i2c_service_write(&s, 0xf3, 0));
    s3_i2c_service_stop(&s, 0);
    for (i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_assert_true(s3_i2c_service_address(&s, false, false, 0));
        g_assert_true(s3_i2c_service_write(&s, 0xe6, 0));
        g_assert_false(s3_i2c_service_write(&s, invalid[i], 0));
        g_assert_false(s3_i2c_service_write(&s, 0x3a, 0));
        s3_i2c_service_stop(&s, 0);
        g_assert_cmpuint(read_user(&s, 0), ==, 0x3a);
    }
    /* Incomplete user-register write cannot consume a later transaction. */
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0xe6, 0));
    s3_i2c_service_stop(&s, 0);
    g_assert_cmpuint(read_user(&s, 0), ==, 0x3a);
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0xf3, 0));
    g_assert_true(s3_i2c_service_address(&s, false, true, 1));
    g_assert_false(s3_i2c_service_write(&s, 0xe7, 1));
    g_assert_true(s.conversion_pending);
    s3_i2c_service_cancel(&s);
    g_assert_true(s3_i2c_service_address(&s, true, false, 85 * MS));
    g_assert_false(s3_i2c_service_write(&s, 0xfe, 85 * MS));
    g_assert_true(s3_i2c_service_read(&s, &byte, 85 * MS));
}

static void eeprom_pointer(S3I2CService *s, uint8_t pointer, int64_t now,
                           bool stop)
{
    g_assert_true(s3_i2c_service_address(s, false, false, now));
    g_assert_true(s3_i2c_service_write(s, pointer, now));
    if (stop) {
        s3_i2c_service_stop(s, now);
        g_assert_true(s->stopped_after_pointer);
    }
    g_assert_true(s3_i2c_service_address(s, true, !stop, now));
}

static void test_eeprom_pointer_final_nack(void)
{
    S3I2CService s;
    uint8_t byte = 0;
    unsigned int stopped;

    s3_i2c_service_init(&s, S3_I2C_EEPROM);
    /* Reading the freshly erased device wraps at the full-array boundary. */
    g_assert_true(s3_i2c_service_address(&s, true, false, 0));
    for (stopped = 0; stopped < 300; stopped++) {
        g_assert_true(s3_i2c_service_read(&s, &byte, 0));
        g_assert_cmpuint(byte, ==, 0xff);
        s3_i2c_service_read_ack(&s, false);
    }
    g_assert_cmpuint(s.pointer, ==, 44);
    s3_i2c_service_stop(&s, 0);
    for (stopped = 0; stopped < 2; stopped++) {
        eeprom_pointer(&s, 0xfe, 0, stopped);
        g_assert_cmpuint(s.pointer, ==, 0xfe);
        g_assert_true(s3_i2c_service_read(&s, &byte, 0));
        s3_i2c_service_read_ack(&s, false);
        g_assert_true(s3_i2c_service_read(&s, &byte, 0));
        g_assert_cmpuint(s.pointer, ==, 0);
        s3_i2c_service_read_ack(&s, true);
        /* ACK after final NACK must not reopen the stream. */
        s3_i2c_service_read_ack(&s, false);
        byte = 0x5a;
        g_assert_false(s3_i2c_service_read(&s, &byte, 0));
        g_assert_cmpuint(byte, ==, 0x5a);
        g_assert_cmpuint(s.pointer, ==, 0);
        s3_i2c_service_stop(&s, 0);
        g_assert_true(s.final_nack);
        g_assert_true(s3_i2c_service_address(&s, true, false, 0));
        g_assert_false(s.final_nack);
        g_assert_true(s3_i2c_service_read(&s, &byte, 0));
        g_assert_cmpuint(s.pointer, ==, 1);
        s3_i2c_service_stop(&s, 0);
    }
}

static void test_eeprom_page_restart_stop(void)
{
    S3I2CService s;
    uint8_t expected[16];
    uint8_t byte;
    unsigned int i;
    int64_t ready = 5 * MS + 7;

    memset(expected, 0xff, sizeof(expected));
    s3_i2c_service_init(&s, S3_I2C_EEPROM);
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x0e, 0));
    /* Above the S3 32-byte FIFO: last write to each page slot wins. */
    for (i = 0; i < 40; i++) {
        uint8_t value = 0x40 + i;

        g_assert_true(s3_i2c_service_write(&s, value, 0));
        expected[(14 + i) & 15] = value;
    }
    g_assert_cmpuint(s.write_count, ==, 40);
    g_assert_cmpuint(s.pointer, ==, 6);
    g_assert_true(s3_i2c_service_address(&s, true, true, 0));
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, 0);
    g_assert_true(s3_i2c_service_read(&s, &byte, 0));
    g_assert_cmpuint(byte, ==, 0xff); /* Not committed by repeated START. */
    s3_i2c_service_stop(&s, 7);
    g_assert_cmpuint(s.page_mask, ==, 0);
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, ready);
    g_assert_false(s3_i2c_service_address(&s, false, false, 7));
    g_assert_false(s3_i2c_service_address(&s, true, false, ready - 1));
    eeprom_pointer(&s, 0, ready, false);
    for (i = 0; i < 32; i++) {
        g_assert_true(s3_i2c_service_read(&s, &byte, ready));
        g_assert_cmpuint(byte, ==, i < 16 ? expected[i] : 0xff);
        s3_i2c_service_read_ack(&s, i == 31);
    }
    s3_i2c_service_stop(&s, ready);
    /* A read-only STOP must not start another EEPROM write cycle. */
    g_assert_true(s3_i2c_service_address(&s, true, false, ready));
}

static void test_eeprom_full_array(void)
{
    S3I2CService s;
    int64_t now = 0;
    unsigned int page, i;
    uint8_t byte;

    s3_i2c_service_init(&s, S3_I2C_EEPROM);
    for (page = 0; page < 16; page++) {
        g_assert_true(s3_i2c_service_address(&s, false, false, now));
        g_assert_true(s3_i2c_service_write(&s, page * 16, now));
        for (i = 0; i < 16; i++) {
            g_assert_true(s3_i2c_service_write(&s, (page * 16 + i) ^ 0xa7, now));
        }
        s3_i2c_service_stop(&s, now);
        now += 5 * MS;
    }
    eeprom_pointer(&s, 0xf0, now, true);
    /* 300-byte stream crosses page, 32-byte FIFO and array boundaries. */
    for (i = 0; i < 300; i++) {
        g_assert_true(s3_i2c_service_read(&s, &byte, now));
        g_assert_cmpuint(byte, ==, ((0xf0 + i) & 0xff) ^ 0xa7);
        s3_i2c_service_read_ack(&s, i == 299);
    }
    g_assert_cmpuint(s.pointer, ==, (0xf0 + 300) & 0xff);
    g_assert_false(s3_i2c_service_read(&s, &byte, now));
}

static void test_eeprom_cancel_busy(void)
{
    S3I2CService s;
    uint8_t byte;

    s3_i2c_service_init(&s, S3_I2C_EEPROM);
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x9f, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x12, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x34, 0));
    g_assert_cmpuint(s.pointer, ==, 0x91);
    /* A repeated write address cannot overwrite the pending page base. */
    g_assert_false(s3_i2c_service_address(&s, false, true, 0));
    s3_i2c_service_cancel(&s);
    g_assert_cmpuint(s.pointer, ==, 0x91);
    g_assert_cmpuint(s.page_mask, ==, 0);
    s3_i2c_service_stop(&s, 0);
    eeprom_pointer(&s, 0x90, 0, false);
    g_assert_true(s3_i2c_service_read(&s, &byte, 0));
    g_assert_cmpuint(byte, ==, 0xff);
    s3_i2c_service_stop(&s, 0);
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x90, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x56, 0));
    s3_i2c_service_stop(&s, 0);
    s3_i2c_service_cancel(&s);
    g_assert_false(s3_i2c_service_address(&s, true, false, 5 * MS - 1));
    eeprom_pointer(&s, 0x90, 5 * MS, true);
    g_assert_true(s3_i2c_service_read(&s, &byte, 5 * MS));
    g_assert_cmpuint(byte, ==, 0x56);
    s3_i2c_service_stop(&s, 5 * MS);
    /* Component init, unlike bus cancel/STOP, restores erased memory. */
    s3_i2c_service_init(&s, S3_I2C_EEPROM);
    eeprom_pointer(&s, 0x90, 0, true);
    g_assert_true(s3_i2c_service_read(&s, &byte, 0));
    g_assert_cmpuint(byte, ==, 0xff);
}

static void test_instances_and_transaction_guards(void)
{
    S3I2CService first, second;
    uint8_t frame[3];
    uint8_t byte;

    s3_i2c_service_init(&first, S3_I2C_SHT21);
    s3_i2c_service_init(&second, S3_I2C_SHT21);
    measure(&first, 0xf3, 0, frame);
    g_assert_cmpuint(first.sample_number, ==, 1);
    g_assert_cmpuint(second.sample_number, ==, 0);
    write_user(&first, 0xbb, 85 * MS);
    g_assert_cmpuint(read_user(&second, 85 * MS), ==, 0x3a);
    s3_i2c_service_init(&first, S3_I2C_EEPROM);
    s3_i2c_service_init(&second, S3_I2C_EEPROM);
    g_assert_false(s3_i2c_service_write(&first, 0, 0));
    g_assert_true(s3_i2c_service_address(&first, false, false, 0));
    g_assert_true(s3_i2c_service_write(&first, 0, 0));
    g_assert_true(s3_i2c_service_write(&first, 0x12, 0));
    /* Missing repeated START must not silently STOP/commit the first page. */
    g_assert_false(s3_i2c_service_address(&first, true, false, 0));
    g_assert_true(first.addressed);
    g_assert_cmpuint(first.page_mask, !=, 0);
    s3_i2c_service_stop(&first, 0);
    g_assert_true(s3_i2c_service_address(&second, true, false, 0));
    g_assert_true(s3_i2c_service_read(&second, &byte, 0));
    g_assert_cmpuint(byte, ==, 0xff);
    g_assert_false(s3_i2c_service_write(&second, 0x12, 0));
}

static void test_deadline_saturation(void)
{
    S3I2CService s;
    uint8_t frame[3];
    int64_t now = INT64_MAX - MS;

    s3_i2c_service_init(&s, S3_I2C_SHT21);
    g_assert_true(s3_i2c_service_address(&s, false, false, now));
    g_assert_true(s3_i2c_service_write(&s, 0xe3, now));
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, INT64_MAX);
    g_assert_true(s3_i2c_service_address(&s, true, true, now));
    read_frame(&s, INT64_MAX, frame);
    s3_i2c_service_init(&s, S3_I2C_EEPROM);
    g_assert_true(s3_i2c_service_address(&s, false, false, now));
    g_assert_true(s3_i2c_service_write(&s, 0xff, now));
    g_assert_true(s3_i2c_service_write(&s, 0x42, now));
    s3_i2c_service_stop(&s, now);
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, INT64_MAX);
    g_assert_false(s3_i2c_service_address(&s, true, false, INT64_MAX - 1));
    g_assert_true(s3_i2c_service_address(&s, true, false, INT64_MAX));
}

static void test_sht_power_reset(void)
{
    S3I2CService s;
    uint8_t frame[3];
    s3_i2c_service_init(&s, S3_I2C_SHT21);
    write_user(&s, 0x3e, 0);
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0xf3, 0));
    s3_i2c_service_stop(&s, 0);
    g_assert_true(s.conversion_pending);
    s3_i2c_service_power_reset(&s, 2 * MS);
    g_assert_false(s.conversion_pending);
    g_assert_cmpuint(s.user_register, ==, 0x3a);
    g_assert_false(s3_i2c_service_address(&s, false, false, 17 * MS - 1));
    g_assert_false(s3_i2c_service_address(&s, true, false, 17 * MS));
    g_assert_cmpuint(read_user(&s, 17 * MS), ==, 0x3a);
    measure(&s, 0xf3, 17 * MS, frame);
    g_assert_cmpuint(frame[0], ==, 0x68);
    g_assert_cmpuint(frame[1], ==, 0x30);
    s3_i2c_service_power_reset(&s, INT64_MAX - 4 * MS);
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, INT64_MAX);
}

static void test_eeprom_power_reset(void)
{
    S3I2CService s;
    uint8_t byte;
    s3_i2c_service_init(&s, S3_I2C_EEPROM);
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x90, 0));
    g_assert_true(s3_i2c_service_write(&s, 0xa5, 0));
    s3_i2c_service_stop(&s, 0);
    s3_i2c_service_power_reset(&s, MS);
    eeprom_pointer(&s, 0x90, MS, true);
    g_assert_true(s3_i2c_service_read(&s, &byte, MS));
    g_assert_cmpuint(byte, ==, 0xa5);
    s3_i2c_service_read_ack(&s, true);
    s3_i2c_service_stop(&s, MS);
    g_assert_true(s3_i2c_service_address(&s, false, false, 2 * MS));
    g_assert_true(s3_i2c_service_write(&s, 0x90, 2 * MS));
    g_assert_true(s3_i2c_service_write(&s, 0x5a, 2 * MS));
    s3_i2c_service_power_reset(&s, 3 * MS);
    g_assert_cmpuint(s.page_mask, ==, 0);
    eeprom_pointer(&s, 0x90, 3 * MS, true);
    g_assert_true(s3_i2c_service_read(&s, &byte, 3 * MS));
    g_assert_cmpuint(byte, ==, 0xa5);
}

/*
 * Stateful register-bank fixture for the external dispatch boundary only.
 * This is not an OV2640 model or evidence of electrical/camera fidelity.
 * Bytes come from writes to the bank and an advancing register pointer.
 */
typedef struct RegisterBank {
    uint8_t bank[256], pointer;
    bool pointer_pending, reading, nack_address, nack_write, nack_read;
    bool last_restart, last_nack;
    unsigned int addresses, writes, reads, acks, stops, cancels, resets;
    int64_t now, ready;
} RegisterBank;

static bool bank_address(void *opaque, bool reading, bool restart, int64_t now)
{
    RegisterBank *b = opaque;

    b->addresses++;
    b->now = now;
    b->last_restart = restart;
    if (b->nack_address || now < b->ready) {
        return false;
    }
    b->reading = reading;
    b->pointer_pending = !reading;
    return true;
}

static bool bank_write(void *opaque, uint8_t byte, int64_t now)
{
    RegisterBank *b = opaque;

    b->writes++;
    b->now = now;
    if (b->nack_write || now < b->ready) {
        return false;
    }
    if (b->pointer_pending) {
        b->pointer = byte;
        b->pointer_pending = false;
    } else {
        b->bank[b->pointer++] = byte;
    }
    return true;
}

static bool bank_read(void *opaque, uint8_t *byte, int64_t now)
{
    RegisterBank *b = opaque;

    b->reads++;
    b->now = now;
    if (b->nack_read || now < b->ready) {
        return false;
    }
    *byte = b->bank[b->pointer++];
    return true;
}

static void bank_read_ack(void *opaque, bool nack)
{
    RegisterBank *b = opaque;

    b->acks++;
    b->last_nack = nack;
}

static void bank_stop(void *opaque, int64_t now)
{
    RegisterBank *b = opaque;

    b->stops++;
    b->now = now;
    b->reading = false;
    b->pointer_pending = false;
}

static void bank_cancel(void *opaque)
{
    RegisterBank *b = opaque;

    b->cancels++;
    b->reading = false;
    b->pointer_pending = false;
}

static void bank_power_reset(void *opaque, int64_t now)
{
    RegisterBank *b = opaque;

    /* The wrapper must cancel the transaction before resetting the owner. */
    g_assert_false(b->reading);
    g_assert_false(b->pointer_pending);
    b->resets++;
    memset(b->bank, 0, sizeof(b->bank));
    b->pointer = 0;
    b->now = now;
    b->ready = now + 3 * MS;
}

static int64_t bank_ready_ns(void *opaque)
{
    RegisterBank *b = opaque;

    return b->ready;
}

static const S3I2CServiceOps bank_ops = {
    .address = bank_address,
    .write = bank_write,
    .read = bank_read,
    .read_ack = bank_read_ack,
    .stop = bank_stop,
    .cancel = bank_cancel,
    .power_reset = bank_power_reset,
    .ready_ns = bank_ready_ns,
};

static void test_external_mandatory_callbacks(void)
{
    unsigned int missing;

    for (missing = 0; missing < 18; missing++) {
        S3I2CService s;
        RegisterBank b = { 0 }, saved;
        S3I2CServiceOps incomplete = bank_ops;
        uint8_t byte = 0xa5;

        switch (missing % 9) {
        case 0: incomplete.address = NULL; break;
        case 1: incomplete.write = NULL; break;
        case 2: incomplete.read = NULL; break;
        case 3: incomplete.read_ack = NULL; break;
        case 4: incomplete.stop = NULL; break;
        case 5: incomplete.cancel = NULL; break;
        case 6: incomplete.power_reset = NULL; break;
        case 7: incomplete.ready_ns = NULL; break;
        }
        /* Fresh poisoned storage and stale built-in state both fail closed. */
        if (missing < 9) {
            memset(&s, 0xa5, sizeof(s));
        } else {
            s3_i2c_service_init(&s, S3_I2C_EEPROM);
            g_assert_true(s3_i2c_service_address(&s, false, false, 0));
        }
        g_assert_false(s3_i2c_service_init_external(
            &s, missing % 9 == 8 ? NULL : &incomplete, &b));
        saved = b;
        g_assert_false(s3_i2c_service_address(&s, false, false, 1));
        g_assert_false(s3_i2c_service_address(&s, true, true, 1));
        g_assert_false(s3_i2c_service_write(&s, 0x12, 1));
        g_assert_false(s3_i2c_service_read(&s, &byte, 1));
        g_assert_cmpuint(byte, ==, 0xa5);
        s3_i2c_service_read_ack(&s, true);
        s3_i2c_service_stop(&s, 1);
        s3_i2c_service_cancel(&s);
        s3_i2c_service_power_reset(&s, 1);
        g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, INT64_MAX);
        g_assert_cmpmem(&b, sizeof(b), &saved, sizeof(saved));
    }
}

static void test_external_register_transactions(void)
{
    S3I2CService s;
    RegisterBank b = { 0 };
    uint8_t byte = 0xa5;
    unsigned int reads;

    g_assert_true(s3_i2c_service_init_external(&s, &bank_ops, &b));
    g_assert_false(s3_i2c_service_write(&s, 0x20, 1));
    g_assert_false(s3_i2c_service_read(&s, &byte, 1));
    g_assert_cmpuint(b.writes + b.reads, ==, 0);
    g_assert_true(s3_i2c_service_address(&s, false, false, 11));
    g_assert_false(b.last_restart);
    g_assert_cmpint(b.now, ==, 11);
    g_assert_true(s3_i2c_service_write(&s, 0x20, 12));
    g_assert_true(s3_i2c_service_write(&s, 0x71, 13));
    g_assert_true(s3_i2c_service_write(&s, 0xc4, 14));
    g_assert_cmpuint(b.bank[0x20], ==, 0x71);
    g_assert_cmpuint(b.bank[0x21], ==, 0xc4);
    g_assert_cmpint(b.now, ==, 14);
    /* Ordinary START cannot replace an active transfer or call the owner. */
    g_assert_false(s3_i2c_service_address(&s, true, false, 15));
    g_assert_cmpuint(b.addresses, ==, 1);
    g_assert_cmpuint(b.pointer, ==, 0x22);
    g_assert_true(s3_i2c_service_address(&s, false, true, 16));
    g_assert_true(b.last_restart);
    g_assert_true(s3_i2c_service_write(&s, 0x20, 17));
    g_assert_true(s3_i2c_service_address(&s, true, true, 18));
    g_assert_true(b.reading);
    g_assert_cmpuint(b.stops + b.cancels + b.resets, ==, 0);
    g_assert_false(s3_i2c_service_write(&s, 0xff, 19));
    g_assert_cmpuint(b.writes, ==, 4);
    g_assert_true(s3_i2c_service_read(&s, &byte, 20));
    g_assert_cmpuint(byte, ==, 0x71);
    s3_i2c_service_read_ack(&s, false);
    g_assert_false(b.last_nack);
    g_assert_true(s3_i2c_service_read(&s, &byte, 21));
    g_assert_cmpuint(byte, ==, 0xc4);
    g_assert_cmpint(b.now, ==, 21);
    s3_i2c_service_read_ack(&s, true);
    g_assert_true(b.last_nack);
    g_assert_cmpuint(b.acks, ==, 2);
    reads = b.reads;
    byte = 0xa5;
    /* ACK after final NACK cannot revive reads in this transaction. */
    s3_i2c_service_read_ack(&s, false);
    g_assert_false(s3_i2c_service_read(&s, &byte, 22));
    g_assert_cmpuint(byte, ==, 0xa5);
    g_assert_cmpuint(b.reads, ==, reads);
    s3_i2c_service_stop(&s, 23);
    g_assert_cmpuint(b.stops, ==, 1);
    g_assert_cmpint(b.now, ==, 23);
    g_assert_false(s3_i2c_service_read(&s, &byte, 24));
    /* Pointer-only STOP retains pointer and written registers. */
    g_assert_true(s3_i2c_service_address(&s, false, false, 25));
    g_assert_true(s3_i2c_service_write(&s, 0x21, 26));
    s3_i2c_service_stop(&s, 27);
    g_assert_true(s3_i2c_service_address(&s, true, false, 28));
    g_assert_false(s.final_nack);
    g_assert_true(s3_i2c_service_read(&s, &byte, 29));
    g_assert_cmpuint(byte, ==, 0xc4);
    g_assert_cmpuint(b.pointer, ==, 0x22);
    s3_i2c_service_stop(&s, 30);
}

static void test_external_nacks(void)
{
    S3I2CService s;
    RegisterBank b = { 0 };
    uint8_t byte = 0xa5;

    g_assert_true(s3_i2c_service_init_external(&s, &bank_ops, &b));
    b.nack_address = true;
    g_assert_false(s3_i2c_service_address(&s, false, false, 1));
    g_assert_false(s3_i2c_service_write(&s, 0x40, 2));
    g_assert_false(s3_i2c_service_read(&s, &byte, 2));
    g_assert_cmpuint(b.writes + b.reads, ==, 0);
    b.nack_address = false;
    g_assert_true(s3_i2c_service_address(&s, false, false, 3));
    b.nack_write = true;
    g_assert_false(s3_i2c_service_write(&s, 0x40, 4));
    g_assert_true(b.pointer_pending);
    b.nack_write = false;
    g_assert_true(s3_i2c_service_write(&s, 0x40, 5));
    g_assert_true(s3_i2c_service_write(&s, 0x92, 6));
    b.nack_write = true;
    g_assert_false(s3_i2c_service_write(&s, 0x37, 7));
    g_assert_cmpuint(b.pointer, ==, 0x41);
    g_assert_cmpuint(b.bank[0x41], ==, 0);
    b.nack_write = false;
    g_assert_true(s3_i2c_service_write(&s, 0x37, 8));
    g_assert_true(s3_i2c_service_address(&s, false, true, 9));
    g_assert_true(s3_i2c_service_write(&s, 0x40, 10));
    b.nack_address = true;
    g_assert_false(s3_i2c_service_address(&s, true, true, 11));
    g_assert_false(s3_i2c_service_read(&s, &byte, 12));
    g_assert_cmpuint(b.reads, ==, 0);
    b.nack_address = false;
    g_assert_true(s3_i2c_service_address(&s, true, false, 13));
    b.nack_read = true;
    g_assert_false(s3_i2c_service_read(&s, &byte, 14));
    g_assert_cmpuint(byte, ==, 0xa5);
    g_assert_cmpuint(b.pointer, ==, 0x40);
    b.nack_read = false;
    g_assert_true(s3_i2c_service_read(&s, &byte, 15));
    g_assert_cmpuint(byte, ==, 0x92);
    s3_i2c_service_read_ack(&s, false);
    g_assert_true(s3_i2c_service_read(&s, &byte, 16));
    g_assert_cmpuint(byte, ==, 0x37);
    s3_i2c_service_read_ack(&s, true);
    s3_i2c_service_stop(&s, 17);
    g_assert_cmpuint(b.resets, ==, 0);
}

static void test_external_cancel_power_deadline(void)
{
    S3I2CService s;
    RegisterBank b = { 0 };
    uint8_t byte = 0xa5;

    g_assert_true(s3_i2c_service_init_external(&s, &bank_ops, &b));
    g_assert_true(s3_i2c_service_address(&s, false, false, 0));
    g_assert_true(s3_i2c_service_write(&s, 0xff, 0));
    g_assert_true(s3_i2c_service_write(&s, 0x6b, 0));
    g_assert_cmpuint(b.pointer, ==, 0);
    g_assert_true(s3_i2c_service_write(&s, 0x94, 0));
    b.ready = 7 * MS;
    s3_i2c_service_cancel(&s);
    g_assert_cmpuint(b.cancels, ==, 1);
    g_assert_cmpuint(b.resets + b.stops, ==, 0);
    g_assert_cmpuint(b.bank[0xff], ==, 0x6b);
    g_assert_cmpuint(b.bank[0], ==, 0x94);
    g_assert_cmpuint(b.pointer, ==, 1);
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, 7 * MS);
    g_assert_false(s3_i2c_service_read(&s, &byte, 7 * MS));
    g_assert_false(s3_i2c_service_address(&s, false, false, 7 * MS - 1));
    g_assert_true(s3_i2c_service_address(&s, false, false, 7 * MS));
    g_assert_true(s3_i2c_service_write(&s, 0xff, 7 * MS));
    g_assert_true(s3_i2c_service_address(&s, true, true, 7 * MS));
    g_assert_true(s3_i2c_service_read(&s, &byte, 7 * MS));
    g_assert_cmpuint(byte, ==, 0x6b);
    /* Owner deadline can also block data in an already addressed transfer. */
    b.ready = 8 * MS;
    byte = 0xa5;
    g_assert_false(s3_i2c_service_read(&s, &byte, 8 * MS - 1));
    g_assert_cmpuint(byte, ==, 0xa5);
    g_assert_cmpuint(b.pointer, ==, 0);
    g_assert_true(s3_i2c_service_read(&s, &byte, 8 * MS));
    g_assert_cmpuint(byte, ==, 0x94);
    s3_i2c_service_power_reset(&s, 9 * MS);
    g_assert_cmpuint(b.cancels, ==, 2);
    g_assert_cmpuint(b.resets, ==, 1);
    g_assert_cmpuint(b.stops, ==, 0);
    g_assert_cmpuint(b.pointer, ==, 0);
    g_assert_cmpuint(b.bank[0xff] + b.bank[0], ==, 0);
    g_assert_cmpint(b.now, ==, 9 * MS);
    g_assert_cmpint(s3_i2c_service_ready_ns(&s), ==, 12 * MS);
    g_assert_false(s3_i2c_service_read(&s, &byte, 12 * MS));
    g_assert_false(s3_i2c_service_address(&s, true, false, 12 * MS - 1));
    g_assert_true(s3_i2c_service_address(&s, true, false, 12 * MS));
    g_assert_true(s3_i2c_service_read(&s, &byte, 12 * MS));
    g_assert_cmpuint(byte, ==, 0);
    s3_i2c_service_read_ack(&s, true);
    s3_i2c_service_stop(&s, 12 * MS);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/esp32s3/i2c-service/external/mandatory-callbacks",
                    test_external_mandatory_callbacks);
    g_test_add_func("/esp32s3/i2c-service/external/register-transactions",
                    test_external_register_transactions);
    g_test_add_func("/esp32s3/i2c-service/external/nacks", test_external_nacks);
    g_test_add_func("/esp32s3/i2c-service/external/cancel-power-deadline",
                    test_external_cancel_power_deadline);
    g_test_add_func("/esp32s3/i2c-service/sht21/power-reset", test_sht_power_reset);
    g_test_add_func("/esp32s3/i2c-service/eeprom/power-reset", test_eeprom_power_reset);
    g_test_add_func("/esp32s3/i2c-service/sht21/no-hold", test_sht_no_hold);
    g_test_add_func("/esp32s3/i2c-service/sht21/hold-cancel", test_sht_hold_cancel);
    g_test_add_func("/esp32s3/i2c-service/sht21/resolution", test_sht_resolution);
    g_test_add_func("/esp32s3/i2c-service/sht21/otp-reset", test_sht_otp_reset);
    g_test_add_func("/esp32s3/i2c-service/sht21/invalid-writes", test_sht_invalid_writes);
    g_test_add_func("/esp32s3/i2c-service/eeprom/pointer-final-nack",
                    test_eeprom_pointer_final_nack);
    g_test_add_func("/esp32s3/i2c-service/eeprom/page-restart-stop",
                    test_eeprom_page_restart_stop);
    g_test_add_func("/esp32s3/i2c-service/eeprom/full-array", test_eeprom_full_array);
    g_test_add_func("/esp32s3/i2c-service/eeprom/cancel-busy", test_eeprom_cancel_busy);
    g_test_add_func("/esp32s3/i2c-service/instances-guards",
                    test_instances_and_transaction_guards);
    g_test_add_func("/esp32s3/i2c-service/deadline-saturation", test_deadline_saturation);
    g_test_add_func("/esp32s3/i2c-service/sht21/final-nack-frame-end",
                    test_sht_final_nack_and_frame_end);
    return g_test_run();
}
