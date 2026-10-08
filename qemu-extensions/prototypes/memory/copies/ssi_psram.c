/*
 * ESP-PSRAM basic emulation
 *
 * Copyright (c) 2021-2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 *
 * Native 8 MiB SPI/QPI qualification: see ../ssi-contract.json.
 * Based on official hw/misc/ssi_psram.c, commit
 * 40edccac415693c5130f91c01d84176ae6008566.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "hw/misc/ssi_psram.h"

#define PSRAM_SIZE_BYTES (8U * 1024 * 1024)
#define PSRAM_ADDR_MASK  (PSRAM_SIZE_BYTES - 1)

typedef enum PsramCMD {
    READ             = 0x03,
    FAST_READ        = 0x0b,
    FAST_READ_QUAD   = 0xeb,
    WRITE            = 0x02,
    QUAD_WRITE       = 0x38,
    ENTER_QUAD_MODE  = 0x35,
    EXIT_QUAD_MODE   = 0xf5,
    RESET_ENABLE     = 0x66,
    RESET            = 0x99,
    SET_BURST_LENGTH = 0xc0,
    READ_ID          = 0x9f,
} PsramCMD;

/*
 * Figure 6-7: MF=0x0d, KGD=0x5d, followed by 48-bit EID, MSB first.
 * IDF decodes EID[47:45]=010 as 64 Mbit and EID[41]=1 as non-2T.
 * Unspecified manufacturing bits are the model's zero identity, not a
 * captured hardware serial number or a fabricated larger-density ID.
 */
static const uint8_t psram_id[] = {
    0x0d, 0x5d, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static bool psram_is_write_command(const SsiPsramState *s)
{
    return s->command == WRITE || s->command == QUAD_WRITE;
}

static uint32_t psram_data_address(const SsiPsramState *s, uint64_t offset)
{
    /* The physical chip decodes only A[22:0], not the wire's A23. */
    if (s->wrap32) {
        return (s->addr & (PSRAM_ADDR_MASK & ~31U)) |
               ((s->addr + offset) & 31U);
    }
    return (s->addr + offset) & PSRAM_ADDR_MASK;
}

static void psram_finish_write(SsiPsramState *s)
{
    uint32_t start, first;

    if (s->state != ST_PROCESSING || !psram_is_write_command(s) ||
        !s->byte_count) {
        return;
    }
    start = psram_data_address(s, 0);
    if (s->wrap32) {
        memory_region_set_dirty(&s->data_mr, start & ~31U, 32);
    } else if (s->byte_count >= PSRAM_SIZE_BYTES) {
        memory_region_set_dirty(&s->data_mr, 0, PSRAM_SIZE_BYTES);
    } else {
        first = MIN(s->byte_count, PSRAM_SIZE_BYTES - start);
        memory_region_set_dirty(&s->data_mr, start, first);
        if (s->byte_count > first) {
            memory_region_set_dirty(&s->data_mr, 0, s->byte_count - first);
        }
    }
}

static void psram_transaction_clear(SsiPsramState *s)
{
    s->state = ST_IDLE;
    s->command = 0;
    s->addr = 0;
    s->byte_count = 0;
    s->dummy_cycles = 0;
    s->reset_requested = false;
}

static void psram_reject(SsiPsramState *s, const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ssi_psram: %s (command 0x%02x, %s mode)\n",
                  reason, s->command, s->qpi ? "QPI" : "SPI");
    s->state = ST_REJECTED;
}

static void psram_command(SsiPsramState *s, uint8_t command)
{
    bool reset_enabled = s->reset_enabled;

    s->command = command;
    s->reset_enabled = false;
    switch (command) {
    case READ:
    case FAST_READ:
        if (s->qpi) {
            psram_reject(s, "serial read unavailable in qualified QPI protocol");
            return;
        }
        /* fall through */
    case FAST_READ_QUAD:
    case WRITE:
    case QUAD_WRITE:
        s->state = ST_CMD_ADDR0;
        return;
    case READ_ID:
        if (s->qpi) {
            psram_reject(s, "Read ID is available only in SPI mode");
            return;
        }
        s->state = ST_CMD_ADDR0;
        return;
    case ENTER_QUAD_MODE:
        if (s->qpi) {
            psram_reject(s, "Enter QPI is available only in SPI mode");
            return;
        }
        break;
    case EXIT_QUAD_MODE:
        if (!s->qpi) {
            psram_reject(s, "Exit QPI is available only in QPI mode");
            return;
        }
        break;
    case RESET:
        s->reset_requested = reset_enabled;
        break;
    case RESET_ENABLE:
    case SET_BURST_LENGTH:
        break;
    default:
        psram_reject(s, "unsupported command");
        return;
    }
    /* One-byte control commands take effect on CE# rising, not on opcode. */
    s->state = ST_CONTROL;
}

static uint32_t psram_transfer(SSIPeripheral *dev, uint32_t value)
{
    SsiPsramState *s = SSI_PSRAM(dev);
    uint8_t *ram;
    uint32_t address;

    switch (s->state) {
    case ST_IDLE:
        psram_command(s, value & 0xff);
        break;
    case ST_CMD_ADDR0:
        s->addr = value & 0xff;
        s->state = ST_CMD_ADDR1;
        break;
    case ST_CMD_ADDR1:
        s->addr = (s->addr << 8) | (value & 0xff);
        s->state = ST_CMD_ADDR2;
        break;
    case ST_CMD_ADDR2:
        s->addr = (s->addr << 8) | (value & 0xff);
        if (s->command == READ_ID) {
            s->state = ST_READ_ID;
        } else if (s->command == FAST_READ || s->command == FAST_READ_QUAD) {
            /*
             * Official SPI1 serializes dummy clocks as ceil(clocks / 8)
             * SSI bytes even for quad transactions. Both the SPI 0x0b
             * eight clocks and 0xeb six clocks therefore consume one
             * SSI transfer. SSI carries no edge or lane-width metadata.
             */
            s->dummy_cycles = 1;
            s->state = ST_DUMMY_CYCLE;
        } else {
            s->state = ST_PROCESSING;
        }
        break;
    case ST_DUMMY_CYCLE:
        if (--s->dummy_cycles == 0) {
            s->state = ST_PROCESSING;
        }
        break;
    case ST_PROCESSING:
        address = psram_data_address(s, s->byte_count++);
        ram = memory_region_get_ram_ptr(&s->data_mr);
        if (psram_is_write_command(s)) {
            ram[address] = value & 0xff;
        } else {
            return ram[address];
        }
        break;
    case ST_READ_ID:
        if (s->byte_count < ARRAY_SIZE(psram_id)) {
            return psram_id[s->byte_count++];
        }
        psram_reject(s, "Read ID exceeded the documented 64-bit response");
        break;
    case ST_CONTROL:
        psram_reject(s, "control command contains extra bytes");
        s->reset_requested = false;
        break;
    case ST_REJECTED:
        break;
    }
    /* SSI's undriven/non-data contribution; never an unbacked RAM read. */
    return 0;
}

static int psram_cs(SSIPeripheral *dev, bool level)
{
    SsiPsramState *s = SSI_PSRAM(dev);

    /* SSI passes the electrical level: true is CE# high (deselected). */
    if (!level) {
        return 0;
    }
    psram_finish_write(s);
    if (s->state == ST_CONTROL) {
        switch (s->command) {
        case ENTER_QUAD_MODE:
            s->qpi = true;
            break;
        case EXIT_QUAD_MODE:
            s->qpi = false;
            break;
        case RESET_ENABLE:
            s->reset_enabled = true;
            break;
        case RESET:
            if (s->reset_requested) {
                s->qpi = false;
                s->wrap32 = false;
            }
            break;
        case SET_BURST_LENGTH:
            s->wrap32 = !s->wrap32;
            break;
        }
    }
    psram_transaction_clear(s);
    return 0;
}

static void psram_reset_hold(Object *obj, ResetType type)
{
    SsiPsramState *s = SSI_PSRAM(obj);

    /* A SoC reset is not a PSRAM power cycle or a wire 66h/99h reset. */
    psram_finish_write(s);
    psram_transaction_clear(s);
    s->reset_enabled = false;
}

static void psram_realize(SSIPeripheral *dev, Error **errp)
{
    SsiPsramState *s = SSI_PSRAM(dev);

    if (s->is_octal) {
        error_setg(errp, "ssi_psram: OPI DDR/DQS, mode-register latency and "
                   "burst semantics are not qualified by this SSI model");
        return;
    }
    if (s->size_mbytes != 8) {
        error_setg(errp, "ssi_psram: only genuinely backed 8 MiB SPI/QPI "
                   "PSRAM is supported (requested %" PRIu32 " MiB)",
                   s->size_mbytes);
        return;
    }
    if (!memory_region_init_ram(&s->data_mr, OBJECT(s), "psram.memory_region",
                                PSRAM_SIZE_BYTES, errp)) {
        return;
    }
    s->qpi = false;
    s->wrap32 = false;
    s->reset_enabled = false;
    psram_transaction_clear(s);
}

static Property psram_properties[] = {
    DEFINE_PROP_BOOL("is_octal", SsiPsramState, is_octal, false),
    DEFINE_PROP_UINT32("size_mbytes", SsiPsramState, size_mbytes, 8),
    DEFINE_PROP_END_OF_LIST(),
};

static void psram_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->transfer = psram_transfer;
    k->set_cs = psram_cs;
    k->cs_polarity = SSI_CS_LOW;
    k->realize = psram_realize;
    rc->phases.hold = psram_reset_hold;
    device_class_set_props(dc, psram_properties);
}

static const TypeInfo psram_info = {
    .name          = TYPE_SSI_PSRAM,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SsiPsramState),
    .class_init    = psram_class_init,
};

static void psram_register_types(void)
{
    type_register_static(&psram_info);
}

type_init(psram_register_types)
