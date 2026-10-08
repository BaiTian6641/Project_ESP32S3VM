/*
 * ESP32-S3 RTC IO MUX
 *
 * 22 RTC pads (RTC_GPIO0..21 = GPIO0..21) with W1TS/W1TC atomic operations
 * on OUT/ENABLE/STATUS (all in bits [31:10]), per-pin interrupt config
 * (RTC_GPIO_PINn: PAD_DRIVER, INT_TYPE edge/level detection) and per-pad
 * configuration (pulls, drive strength, RTC mux ownership) with TRM reset
 * defaults.  External drivers connect to the named "rtcio-in" lines; the
 * resolved pad levels are exposed on "rtcio-out".
 *
 * Boundaries (documented, not modelled): deep-sleep hold, touch/analog
 * sense path, RTC power domains and the RTC_CNTL-side interrupt
 * aggregation (RTC_CNTL_INT_ST_RTC_GPIO).
 *
 * Copyright (c) 2024-2026 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 ESP32S3VM project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32s3_rtc_io.h"
#if defined(__has_include)
#if __has_include("hw/xtensa/esp32s3_reset_domain.h")
#include "hw/xtensa/esp32s3_reset_domain.h"
#define ESP32S3_RTC_IO_RESET_DOMAIN 1
#endif
#endif

#define RTC_IO_DEBUG 0

static void esp32s3_rtc_io_update_all(ESP32S3RtcIoState *s);
static void esp32s3_rtc_io_notify_observer(ESP32S3RtcIoState *s);

/* Pad config fields common to every pad register */
#define RTC_IO_PAD_FUN_IE     (1u << 13)
#define RTC_IO_PAD_MUX_SEL    (1u << 19)

static inline int rtc_io_pin_int_type(ESP32S3RtcIoState *s, int n)
{
    return extract32(s->pin[n], 7, 3);
}

static inline int rtc_io_pin_open_drain(ESP32S3RtcIoState *s, int n)
{
    return extract32(s->pin[n], 2, 1);
}

static inline int rtc_io_pad_ie(ESP32S3RtcIoState *s, int n)
{
    return s->pad[n] & RTC_IO_PAD_FUN_IE;
}

/* RUE/RDE are the RTC pad pull-up/down enables (IDF rtc_gpio_pullup_en) */
static inline int rtc_io_pad_rue(ESP32S3RtcIoState *s, int n)
{
    return s->pad[n] & (1u << 27);
}

static inline int rtc_io_pad_rde(ESP32S3RtcIoState *s, int n)
{
    return s->pad[n] & (1u << 28);
}

static inline int rtc_io_data_bit(uint32_t reg, int n)
{
    return (reg >> (ESP32S3_RTC_IO_DATA_SHIFT + n)) & 1;
}

static uint64_t esp32s3_rtc_io_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3RtcIoState *s = ESP32S3_RTC_IO(opaque);

    /* RTC GPIO pin config registers */
    if (addr >= RTC_IO_PIN_OFF(0) && addr <= RTC_IO_PIN_LAST_OFF &&
        ((addr - RTC_IO_PIN_OFF(0)) % 4) == 0) {
        return s->pin[(addr - RTC_IO_PIN_OFF(0)) / 4];
    }

    /* Per-pad config registers */
    for (int n = 0; n < ESP32S3_RTC_IO_GPIO_COUNT; n++) {
        if (addr == RTC_IO_PAD_OFF(n)) {
            return s->pad[n];
        }
    }

    switch (addr) {
    case RTC_IO_OUT_OFF:
        return s->out;
    case RTC_IO_OUT_W1TS_OFF:
    case RTC_IO_OUT_W1TC_OFF:
        return 0;  /* write-only */
    case RTC_IO_ENABLE_OFF:
        return s->enable;
    case RTC_IO_ENABLE_W1TS_OFF:
    case RTC_IO_ENABLE_W1TC_OFF:
        return 0;
    case RTC_IO_STATUS_OFF:
        return s->status;
    case RTC_IO_STATUS_W1TS_OFF:
    case RTC_IO_STATUS_W1TC_OFF:
        return 0;
    case RTC_IO_IN_OFF:
        return s->in;
    case RTC_IO_DEBUG_SEL_OFF:
        return s->debug_sel;
    case RTC_IO_EXT_WAKEUP0_OFF:
        return s->ext_wakeup0;
    case RTC_IO_XTL_EXT_CTR_OFF:
        return s->xtl_ext_ctr;
    case RTC_IO_SAR_I2C_IO_OFF:
        return s->sar_i2c_io;
    case RTC_IO_TOUCH_CTRL_OFF:
        return s->touch_ctrl;
    case RTC_IO_DATE_OFF:
        return ESP32S3_RTC_IO_DATE_VERSION;
    default:
        if (RTC_IO_DEBUG) {
            qemu_log_mask(LOG_UNIMP,
                          "esp32s3_rtc_io: unhandled read 0x%03" HWADDR_PRIx "\n",
                          addr);
        }
        return 0;
    }
}

static void esp32s3_rtc_io_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned int size)
{
    ESP32S3RtcIoState *s = ESP32S3_RTC_IO(opaque);
    uint32_t v = (uint32_t)value & ESP32S3_RTC_IO_DATA_MASK;

    /* RTC GPIO pin config registers */
    if (addr >= RTC_IO_PIN_OFF(0) && addr <= RTC_IO_PIN_LAST_OFF &&
        ((addr - RTC_IO_PIN_OFF(0)) % 4) == 0) {
        s->pin[(addr - RTC_IO_PIN_OFF(0)) / 4] = (uint32_t)value & 0x7FF;
        return;
    }

    /* Per-pad config registers */
    for (int n = 0; n < ESP32S3_RTC_IO_GPIO_COUNT; n++) {
        if (addr == RTC_IO_PAD_OFF(n)) {
            s->pad[n] = (uint32_t)value & 0x7FFFFFFF;
            return;
        }
    }

    switch (addr) {
    case RTC_IO_OUT_OFF:
        s->out = (s->out & ~ESP32S3_RTC_IO_DATA_MASK) | v;
        break;
    case RTC_IO_OUT_W1TS_OFF:
        s->out |= v;
        break;
    case RTC_IO_OUT_W1TC_OFF:
        s->out &= ~v;
        break;
    case RTC_IO_ENABLE_OFF:
        s->enable = (s->enable & ~ESP32S3_RTC_IO_DATA_MASK) | v;
        break;
    case RTC_IO_ENABLE_W1TS_OFF:
        s->enable |= v;
        break;
    case RTC_IO_ENABLE_W1TC_OFF:
        s->enable &= ~v;
        break;
    case RTC_IO_STATUS_OFF:
        s->status |= v;
        break;
    case RTC_IO_STATUS_W1TS_OFF:
        s->status |= v;
        break;
    case RTC_IO_STATUS_W1TC_OFF:
        s->status &= ~v;
        break;
    case RTC_IO_DEBUG_SEL_OFF:
        s->debug_sel = (uint32_t)value & 0x03FFFFFF;
        break;
    case RTC_IO_EXT_WAKEUP0_OFF:
        s->ext_wakeup0 = (uint32_t)value & 0xF8000000;
        break;
    case RTC_IO_XTL_EXT_CTR_OFF:
        s->xtl_ext_ctr = (uint32_t)value & 0xF8000000;
        break;
    case RTC_IO_SAR_I2C_IO_OFF:
        s->sar_i2c_io = (uint32_t)value & 0x000001FF;
        break;
    case RTC_IO_TOUCH_CTRL_OFF:
        s->touch_ctrl = (uint32_t)value & 0x1F;
        break;
    case RTC_IO_DATE_OFF:
        /* Version register — ignore writes */
        break;
    default:
        if (RTC_IO_DEBUG) {
            qemu_log_mask(LOG_UNIMP,
                          "esp32s3_rtc_io: unhandled write 0x%03" HWADDR_PRIx "\n",
                          addr);
        }
        break;
    }

    esp32s3_rtc_io_notify_observer(s);
    esp32s3_rtc_io_update_all(s);
}

/* ================================================================== */
/*  Pad resolution and interrupts                                      */
/* ================================================================== */

uint32_t esp32s3_rtc_io_pad_level(ESP32S3RtcIoState *s, unsigned pad)
{
    if (pad >= ESP32S3_RTC_IO_GPIO_COUNT) {
        return 0;
    }
    return s->line_level[pad];
}

static void esp32s3_rtc_io_update_pad(ESP32S3RtcIoState *s, int n)
{
    int out_val = rtc_io_data_bit(s->out, n);
    int oe = rtc_io_data_bit(s->enable, n);
    bool attached = s->solver_owned || s->solver_attached[n];
    int node;
    int valid;
    int sample;
    int prev = s->sample[n];
    int type = rtc_io_pin_int_type(s, n);
    int event = 0;

    if (oe && !(rtc_io_pin_open_drain(s, n) && out_val == 1)) {
        if (attached && s->net_valid[n]) {
            /* Attached solver level is authoritative; CONTENTION reports
             * raw-desired vs solved sample */
            s->diag[n] = (s->ext_level[n] != out_val) ?
                         ESP32S3_RTC_IO_PAD_CONTENTION :
                         ESP32S3_RTC_IO_PAD_DRIVEN;
            node = s->ext_level[n];
            valid = 1;
        } else {
            s->diag[n] = ESP32S3_RTC_IO_PAD_DRIVEN;
            node = out_val;
            valid = 1;
        }
    } else if (s->net_valid[n]) {
        /* External/solver driver beats the weak pulls */
        node = s->ext_level[n];
        valid = 1;
        s->diag[n] = ESP32S3_GPIO_RTC_PAD_EXTERNAL;
    } else if (rtc_io_pad_rue(s, n) && !attached) {
        node = 1;
        valid = 1;
        s->diag[n] = ESP32S3_RTC_IO_PAD_PULLED_HIGH;
    } else if (rtc_io_pad_rde(s, n) && !attached) {
        node = 0;
        valid = 1;
        s->diag[n] = ESP32S3_RTC_IO_PAD_PULLED_LOW;
    } else if (attached) {
        /* Solver owns the answer: an unresolved report stays UNKNOWN and
         * never falls back to a pull or an implicit floating-0 */
        node = 0;
        valid = 0;
        s->diag[n] = ESP32S3_RTC_IO_PAD_UNKNOWN;
    } else {
        /* Floating: unresolved, reported — never folded into a valid low */
        node = 0;
        valid = 0;
        s->diag[n] = ESP32S3_RTC_IO_PAD_FLOATING;
    }

    if (s->line_level[n] != node) {
        s->line_level[n] = node;
        qemu_set_irq(s->output_lines[n], node);
    }

    if (!(rtc_io_pad_ie(s, n) && valid)) {
        sample = (rtc_io_pad_ie(s, n) && attached) ? s->sample[n] : 0;
    } else {
        sample = node;
    }

    switch (type) {
    case 1: /* rising edge */
        event = !prev && sample;
        break;
    case 2: /* falling edge */
        event = prev && !sample;
        break;
    case 3: /* any edge */
        event = prev != sample;
        break;
    case 4: /* low level */
        event = !sample;
        break;
    case 5: /* high level */
        event = sample;
        break;
    default:
        event = 0;
        break;
    }

    if (event) {
        s->status |= 1u << (ESP32S3_RTC_IO_DATA_SHIFT + n);
    }
    s->sample[n] = sample;

    s->in = 0;
    for (int i = 0; i < ESP32S3_RTC_IO_GPIO_COUNT; i++) {
        s->in |= (uint32_t)s->sample[i] << (ESP32S3_RTC_IO_DATA_SHIFT + i);
    }
}

static void esp32s3_rtc_io_update_all(ESP32S3RtcIoState *s)
{
    for (int n = 0; n < ESP32S3_RTC_IO_GPIO_COUNT; n++) {
        esp32s3_rtc_io_update_pad(s, n);
    }
}

void esp32s3_rtc_io_drive_net(ESP32S3RtcIoState *s, unsigned pad,
                              bool valid, bool level, double volts)
{
    if (pad >= ESP32S3_RTC_IO_GPIO_COUNT) {
        return;
    }
    s->solver_attached[pad] = true;
    if (!valid) {
        s->net_valid[pad] = false;
        s->ext_level[pad] = 0;
        s->diag[pad] = ESP32S3_RTC_IO_PAD_UNKNOWN;
    } else {
        s->net_valid[pad] = true;
        s->ext_level[pad] = (level != 0);
        s->diag[pad] = ESP32S3_GPIO_RTC_PAD_EXTERNAL;
    }
    esp32s3_rtc_io_update_pad(s, (int)pad);
    /* No raw_notify here: the solver caused this update itself. */
}

void esp32s3_rtc_io_set_drive_observer(ESP32S3RtcIoState *s,
                                   ESP32S3RtcIoDriveObserver fn, void *opaque)
{
    s->drive_observer = fn;
    s->observer_opaque = opaque;
}

bool esp32s3_rtc_io_solver_attached(ESP32S3RtcIoState *s, unsigned pad)
{
    return pad < ESP32S3_RTC_IO_GPIO_COUNT ? s->solver_attached[pad] : false;
}

void esp32s3_rtc_io_set_solver_owned(ESP32S3RtcIoState *s, bool owned)
{
    s->solver_owned = owned;
}

static void esp32s3_rtc_io_notify_observer(ESP32S3RtcIoState *s)
{
    if (s->drive_observer) {
        s->drive_observer(s->observer_opaque);
    }
}

ESP32S3RtcIoPadState esp32s3_rtc_io_pad_state(ESP32S3RtcIoState *s, unsigned pad)
{
    return pad < ESP32S3_RTC_IO_GPIO_COUNT ?
           s->diag[pad] : ESP32S3_RTC_IO_PAD_FLOATING;
}

bool esp32s3_rtc_io_net_valid(ESP32S3RtcIoState *s, unsigned pad)
{
    return pad < ESP32S3_RTC_IO_GPIO_COUNT ? s->net_valid[pad] : false;
}

void esp32s3_rtc_io_get_drive_snapshot(ESP32S3RtcIoState *s, unsigned pad,
                                       ESP32S3RtcIoDriveSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    if (pad >= ESP32S3_RTC_IO_GPIO_COUNT) {
        return;
    }

    out->mux_sel = s->pad[pad] & RTC_IO_PAD_MUX_SEL;
    out->out_bit = rtc_io_data_bit(s->out, (int)pad);
    out->out_oe = rtc_io_data_bit(s->enable, (int)pad);
    out->open_drain = rtc_io_pin_open_drain(s, (int)pad);
    out->int_type = rtc_io_pin_int_type(s, (int)pad);
    out->fun_ie = s->pad[pad] & RTC_IO_PAD_FUN_IE;
    out->rue = rtc_io_pad_rue(s, (int)pad);
    out->rde = rtc_io_pad_rde(s, (int)pad);
    out->drv = extract32(s->pad[pad], 29, 2);
    out->slp_sel = extract32(s->pad[pad], 16, 1);
    out->xpd = extract32(s->pad[pad], 20, 1);
    /* hold stays false: deep-sleep hold is a documented boundary */
}

static void esp32s3_rtc_io_input_line(void *opaque, int n, int level)
{
    ESP32S3RtcIoState *s = ESP32S3_RTC_IO(opaque);

    if (n < 0 || n >= ESP32S3_RTC_IO_GPIO_COUNT) {
        return;
    }
    if (s->solver_owned) {
        return; /* solver-owned: only drive_net writes the solved net */
    }
    s->ext_level[n] = (level != 0);
    s->net_valid[n] = true;
    s->diag[n] = ESP32S3_GPIO_RTC_PAD_EXTERNAL;
    esp32s3_rtc_io_update_pad(s, n);
}

/* ================================================================== */
/*  Lifecycle                                                          */
/* ================================================================== */

static const MemoryRegionOps esp32s3_rtc_io_ops = {
    .read  = esp32s3_rtc_io_read,
    .write = esp32s3_rtc_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void esp32s3_rtc_io_reset_hold(Object *obj, ResetType type)
{
    ESP32S3RtcIoState *s = ESP32S3_RTC_IO(obj);

#ifdef ESP32S3_RTC_IO_RESET_DOMAIN
    /* RTC pad state belongs to the RTC domain: CPU-only resets preserve it */
    if (s->soc_reset && !esp32s3_reset_covers_rtc(s->soc_reset)) {
        return;
    }
#endif
    s->out = 0;
    s->enable = 0;
    s->status = 0;
    s->in = 0;

    for (int i = 0; i < ESP32S3_RTC_IO_GPIO_COUNT; i++) {
        s->pin[i] = 0;
        s->pad[i] = ESP32S3_RTC_IO_PAD_DEFAULT;
        s->diag[i] = ESP32S3_RTC_IO_PAD_FLOATING;
        s->net_valid[i] = false;
        s->solver_attached[i] = false;
        s->ext_level[i] = 0;
        s->sample[i] = 0;
        s->line_level[i] = 0;
        qemu_set_irq(s->output_lines[i], 0);
    }

    s->debug_sel = 0;
    s->ext_wakeup0 = 0;
    s->xtl_ext_ctr = 0;
    s->sar_i2c_io = 0;
    s->touch_ctrl = 0;
    esp32s3_rtc_io_notify_observer(s);
}

static void esp32s3_rtc_io_realize(DeviceState *dev, Error **errp)
{
    esp32s3_rtc_io_reset_hold(OBJECT(dev), RESET_TYPE_COLD);
}

static void esp32s3_rtc_io_init(Object *obj)
{
    ESP32S3RtcIoState *s = ESP32S3_RTC_IO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_rtc_io_ops, s,
                          TYPE_ESP32S3_RTC_IO, ESP32S3_RTC_IO_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in_named(DEVICE(obj), esp32s3_rtc_io_input_line,
                            ESP32S3_RTC_IO_INPUT_LINES, ESP32S3_RTC_IO_GPIO_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->output_lines,
                             ESP32S3_RTC_IO_OUTPUT_LINES, ESP32S3_RTC_IO_GPIO_COUNT);
    object_property_add_link(OBJECT(obj), "soc-reset", TYPE_DEVICE,
                             (Object **)&s->soc_reset,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    /* No dedicated IRQ — RTC GPIO interrupts aggregate in RTC_CNTL
     * (RTC_CNTL_INT_ST_RTC_GPIO), which is outside this model. */
}

static void esp32s3_rtc_io_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.hold = esp32s3_rtc_io_reset_hold;
    dc->realize = esp32s3_rtc_io_realize;
    dc->desc = "ESP32-S3 RTC IO MUX";
}

static const TypeInfo esp32s3_rtc_io_info = {
    .name          = TYPE_ESP32S3_RTC_IO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3RtcIoState),
    .instance_init = esp32s3_rtc_io_init,
    .class_init    = esp32s3_rtc_io_class_init,
    .class_size    = sizeof(ESP32S3RtcIoClass),
};

static void esp32s3_rtc_io_register_types(void)
{
    type_register_static(&esp32s3_rtc_io_info);
}

type_init(esp32s3_rtc_io_register_types)
