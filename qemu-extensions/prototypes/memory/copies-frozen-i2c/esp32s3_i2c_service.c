/*
 * Persistent, native-time I2C component services.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SHT21 command/timing/register/CRC profile follows Sensirion's October 2022
 * datasheet, sections 5.4--5.7:
 * https://sensirion.com/media/documents/120BBE4C/63500094/Sensirion_Datasheet_Humidity_Sensor_SHT21.pdf
 * Samples are deterministic, changing ambient values, not a command-response
 * lookup table. Resolution controls both quantization and maximum conversion
 * duration. Heater state is stored but thermal dynamics are not simulated.
 * Electronic ID, undocumented commands and the legacy Sensibus are unsupported.
 *
 * EEPROM is the M24C02-style 256-byte, one-byte-address, 16-byte-page profile:
 * https://www.st.com/resource/en/datasheet/m24c02-w.pdf
 * Initially erased (0xff), without write protection or endurance simulation.
 * Data writes wrap within their starting page, including transfers >32 bytes;
 * sequential reads wrap across the entire 256-byte array. STOP commits the
 * staged page and starts a 5ms address-NACK write cycle. Repeated START never
 * commits: reads see the old array even while a page is staged. For this
 * supported profile any subsequent STOP commits that page, including after a
 * repeated-start read; a second write address while a page is staged NACKs.
 * Pointer-only writes set the current address immediately and preserve it over
 * either STOP or repeated START. cancel discards an uncommitted page but keeps
 * the current address, committed memory and any already-started write cycle.
 *
 * STOP/cancel are bus events, not component power resets. Neither discards an
 * SHT21 conversion or its completed result; init, FE and actual power reset do.
 * FE preserves the heater bit and occupies 15ms. All deadlines are supplied
 * virtual nanoseconds: no timers, allocations, wall clock or sleeps here.
 */
#include "qemu/osdep.h"
#include "hw/i2c/esp32s3_i2c_service.h"

#define SHT_TEMP_HOLD       0xe3
#define SHT_HUMIDITY_HOLD   0xe5
#define SHT_WRITE_USER      0xe6
#define SHT_READ_USER       0xe7
#define SHT_TEMP_NO_HOLD    0xf3
#define SHT_HUMIDITY_NO_HOLD 0xf5
#define SHT_RESET           0xfe
#define SHT_USER_DEFAULT    0x3a
#define NS_PER_MS           INT64_C(1000000)
#define EEPROM_PAGE_SIZE    16

static int64_t deadline_ns(int64_t now_ns, int64_t delay_ns)
{
    return now_ns > INT64_MAX - delay_ns ? INT64_MAX : now_ns + delay_ns;
}

static bool sht_measurement(uint8_t command)
{
    return command == SHT_TEMP_HOLD || command == SHT_TEMP_NO_HOLD ||
           command == SHT_HUMIDITY_HOLD || command == SHT_HUMIDITY_NO_HOLD;
}

static bool sht_hold(uint8_t command)
{
    return command == SHT_TEMP_HOLD || command == SHT_HUMIDITY_HOLD;
}

static uint8_t sht_crc(const uint8_t *bytes)
{
    uint8_t crc = 0;
    unsigned int i, bit;

    for (i = 0; i < 2; i++) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
        }
    }
    return crc;
}

static void sht_start_conversion(S3I2CService *s, int64_t now_ns)
{
    /* Index is the user register's bits 7,0, in that order. */
    static const uint8_t temperature_ms[] = { 85, 22, 43, 11 };
    static const uint8_t humidity_ms[] = { 29, 4, 9, 15 };
    static const uint8_t temperature_bits[] = { 14, 12, 13, 11 };
    static const uint8_t humidity_bits[] = { 12, 8, 10, 11 };
    bool humidity = s->command == SHT_HUMIDITY_HOLD ||
                    s->command == SHT_HUMIDITY_NO_HOLD;
    bool reload = !(s->user_register & 2);
    unsigned int resolution;
    unsigned int bits;
    uint16_t raw;
    int64_t delay;

    if (reload) {
        s->user_register = SHT_USER_DEFAULT | (s->user_register & 4);
    }
    resolution = ((s->user_register >> 6) & 2) | (s->user_register & 1);
    bits = humidity ? humidity_bits[resolution] : temperature_bits[resolution];
    delay = (humidity ? humidity_ms[resolution] : temperature_ms[resolution]) *
            NS_PER_MS + (reload ? INT64_C(2500000) : 0);
    /* Approximately 25--35C and 49--64% RH over a bounded sample sequence. */
    raw = humidity ? 0x7000 + (s->sample_number % 32) * 256 :
                     0x6830 + (s->sample_number % 64) * 64;
    s->sample_number++;
    raw &= (uint16_t)(UINT16_MAX << (16 - bits));
    raw |= humidity ? 2 : 0;
    s->response[0] = raw >> 8;
    s->response[1] = raw;
    s->response[2] = sht_crc(s->response);
    s->response_pos = 0;
    s->conversion_pending = true;
    s->ready_ns = deadline_ns(now_ns, delay);
}

void s3_i2c_service_init(S3I2CService *s, S3I2CServiceKind kind)
{
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->user_register = SHT_USER_DEFAULT;
    if (kind == S3_I2C_EEPROM) {
        memset(s->memory, 0xff, sizeof(s->memory));
    }
}

bool s3_i2c_service_init_external(S3I2CService *s,
                                 const S3I2CServiceOps *ops, void *opaque)
{
    s3_i2c_service_init(s, S3_I2C_EXTERNAL);
    if (!ops || !ops->address || !ops->write || !ops->read || !ops->read_ack ||
        !ops->stop || !ops->cancel || !ops->power_reset || !ops->ready_ns) {
        return false;
    }
    s->ops = ops;
    s->opaque = opaque;
    return true;
}


bool s3_i2c_service_address(S3I2CService *s, bool reading, bool restart,
                            int64_t now_ns)
{
    if (s->kind == S3_I2C_EXTERNAL) {
        if (!s->ops || (s->addressed && !restart)) {
            return false;
        }
        s->addressed = s->ops->address(s->opaque, reading, restart, now_ns);
        s->reading = s->addressed && reading;
        s->final_nack = false;
        return s->addressed;
    }
    /* An ordinary START cannot silently replace an active transaction. */
    if (s->addressed && !restart) {
        return false;
    }
    s->addressed = false;
    s->reading = false;
    s->final_nack = false;
    s->pointer_pending = false;
    s->response_pos = 0;
    s->stopped_after_pointer = false;

    if (s->kind == S3_I2C_EEPROM) {
        if (now_ns < s->ready_ns || (!reading && s->page_mask)) {
            return false;
        }
        if (!reading) {
            s->pointer_pending = true;
            s->write_count = 0;
        }
    } else if (s->kind == S3_I2C_SHT21) {
        s->write_pending = false;
        if (s->command == SHT_RESET && now_ns < s->ready_ns) {
            return false;
        }
        if (reading) {
            if (s->command != SHT_READ_USER && !sht_measurement(s->command)) {
                return false;
            }
            if (sht_measurement(s->command) && now_ns < s->ready_ns &&
                !sht_hold(s->command)) {
                return false;
            }
        } else {
            s->pointer_pending = true; /* Await a fresh command byte. */
            s->write_count = 0;
        }
    } else {
        return false;
    }
    s->addressed = true;
    s->reading = reading;
    return true;
}

bool s3_i2c_service_write(S3I2CService *s, uint8_t byte, int64_t now_ns)
{
    if (!s->addressed || s->reading) {
        return false;
    }
    if (s->kind == S3_I2C_EXTERNAL) {
        return s->ops && s->ops->write(s->opaque, byte, now_ns);
    }
    if (s->kind == S3_I2C_EEPROM) {
        unsigned int offset;

        if (s->pointer_pending) {
            s->pointer = byte;
            s->page_base = byte & ~(EEPROM_PAGE_SIZE - 1);
            s->pointer_pending = false;
            return true;
        }
        offset = s->pointer & (EEPROM_PAGE_SIZE - 1);
        s->page[offset] = byte;
        s->page_mask |= 1u << offset;
        s->pointer = s->page_base | ((offset + 1) & (EEPROM_PAGE_SIZE - 1));
        s->write_pending = true;
        s->write_count++;
        return true;
    }
    if (s->kind != S3_I2C_SHT21) {
        return false;
    }
    if (s->pointer_pending) {
        s->pointer_pending = false;
        if ((!sht_measurement(byte) && byte != SHT_WRITE_USER &&
             byte != SHT_READ_USER && byte != SHT_RESET) ||
            (s->conversion_pending && now_ns < s->ready_ns &&
             byte != SHT_RESET)) {
            return false;
        }
        s->command = byte;
        s->write_count = 1;
        s->response_pos = 0;
        s->conversion_pending = false;
        if (sht_measurement(byte)) {
            sht_start_conversion(s, now_ns);
        } else if (byte == SHT_WRITE_USER) {
            s->write_pending = true;
        } else if (byte == SHT_RESET) {
            s->user_register = SHT_USER_DEFAULT | (s->user_register & 4);
            s->sample_number = 0;
            memset(s->response, 0, sizeof(s->response));
            s->ready_ns = deadline_ns(now_ns, 15 * NS_PER_MS);
        }
        return true;
    }
    if (s->command == SHT_WRITE_USER && s->write_pending) {
        s->write_pending = false;
        /* Reserved bits and the modeled read-only battery flag must survive. */
        if ((byte & 0x78) != (s->user_register & 0x78)) {
            return false;
        }
        s->user_register = byte;
        s->write_count++;
        return true;
    }
    return false;
}

bool s3_i2c_service_read(S3I2CService *s, uint8_t *byte, int64_t now_ns)
{
    if (!s->addressed || !s->reading || s->final_nack) {
        return false;
    }
    if (s->kind == S3_I2C_EXTERNAL) {
        return s->ops && s->ops->read(s->opaque, byte, now_ns);
    }
    if (s->kind == S3_I2C_EEPROM) {
        *byte = s->memory[s->pointer++];
        return true;
    }
    if (s->kind != S3_I2C_SHT21) {
        return false;
    }
    if (s->command == SHT_READ_USER) {
        if (s->response_pos) {
            return false;
        }
        *byte = s->user_register;
        s->response_pos++;
        return true;
    }
    if (!sht_measurement(s->command) || now_ns < s->ready_ns ||
        s->response_pos >= sizeof(s->response)) {
        return false;
    }
    s->conversion_pending = false;
    *byte = s->response[s->response_pos++];
    return true;
}

void s3_i2c_service_read_ack(S3I2CService *s, bool nack)
{
    if (s->kind == S3_I2C_EXTERNAL && s->ops) {
        s->ops->read_ack(s->opaque, nack);
    }
    if (s->addressed && s->reading && nack) {
        s->final_nack = true;
    }
}

void s3_i2c_service_stop(S3I2CService *s, int64_t now_ns)
{
    if (s->kind == S3_I2C_EXTERNAL && s->ops) {
        s->ops->stop(s->opaque, now_ns);
    }
    if (s->kind == S3_I2C_EEPROM) {
        unsigned int i;

        s->stopped_after_pointer = s->addressed && !s->reading &&
                                   !s->pointer_pending && !s->write_count;
        if (s->page_mask) {
            for (i = 0; i < EEPROM_PAGE_SIZE; i++) {
                if (s->page_mask & (1u << i)) {
                    s->memory[s->page_base + i] = s->page[i];
                }
            }
            s->page_mask = 0;
            s->ready_ns = deadline_ns(now_ns, 5 * NS_PER_MS);
        }
    }
    s->addressed = false;
    s->reading = false;
    s->pointer_pending = false;
    s->write_pending = false;
}

void s3_i2c_service_cancel(S3I2CService *s)
{
    if (s->kind == S3_I2C_EXTERNAL && s->ops) {
        s->ops->cancel(s->opaque);
    }
    s->addressed = false;
    s->reading = false;
    s->pointer_pending = false;
    s->write_pending = false;
    s->stopped_after_pointer = false;
    s->page_mask = 0;
}

void s3_i2c_service_power_reset(S3I2CService *s, int64_t now_ns)
{
    if (s->kind == S3_I2C_EXTERNAL) {
        s3_i2c_service_cancel(s);
        s->final_nack = false;
        if (s->ops) {
            s->ops->power_reset(s->opaque, now_ns);
        }
        return;
    }
    s3_i2c_service_cancel(s);
    s->pointer = 0;
    s->write_count = 0;
    s->response_pos = 0;
    s->final_nack = false;
    s->conversion_pending = false;
    if (s->kind == S3_I2C_SHT21) {
        s->command = SHT_RESET;
        s->user_register = SHT_USER_DEFAULT;
        s->sample_number = 0;
        memset(s->response, 0, sizeof(s->response));
        s->ready_ns = deadline_ns(now_ns, 15 * NS_PER_MS);
    } else {
        s->command = 0;
        s->ready_ns = 0;
    }
}

int64_t s3_i2c_service_ready_ns(const S3I2CService *s)
{
    if (s->kind == S3_I2C_EXTERNAL) {
        return s->ops ? s->ops->ready_ns(s->opaque) : INT64_MAX;
    }
    return s->ready_ns;
}
