/*
 * ESP32-S3 APB_SARADC (digital ADC controller) register model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See include/hw/misc/esp32s3_apb_saradc.h for the behavior contract.
 * Register offsets per ESP-IDF v6.1.0 apb_saradc_{reg,struct}.h / TRM 39.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/misc/esp32s3_apb_saradc.h"

/* word indices */
#define R_CTRL              0x00
#define R_CTRL2             0x01
#define R_FILTER_CTRL1      0x02
#define R_FSM_WAIT          0x03
#define R_SAR1_STATUS       0x04 /* RO */
#define R_SAR2_STATUS       0x05 /* RO */
#define R_SAR1_PATT_TAB     0x06 /* 4 words */
#define R_SAR2_PATT_TAB     0x0A /* 4 words */
#define R_ARB_CTRL          0x0E
#define R_FILTER_CTRL0      0x0F
#define R_DATA_STATUS1      0x10 /* RO */
#define R_THRES0_CTRL       0x11
#define R_THRES1_CTRL       0x12
#define R_THRES_CTRL        0x16
#define R_INT_ENA           0x17
#define R_INT_RAW           0x18
#define R_INT_ST            0x19 /* RO */
#define R_INT_CLR           0x1A
#define R_DMA_CONF          0x1B
#define R_CLKM_CONF         0x1C
#define R_DATA_STATUS2      0x1E /* RO */
#define R_APB_CTRL_DATE     0xFF /* RO */

#define CTRL_START          BIT(1)
#define CTRL_START_FORCE    BIT(0)
#define CTRL2_TIMER_EN      BIT(24)
#define DMA_CONF_RESET_FSM  BIT(16)

static void esp32s3_apb_saradc_update_irq(ESP32S3ApbSaradcState *s)
{
    qemu_set_irq(s->irq, (s->regs[R_INT_RAW] & s->regs[R_INT_ENA]) != 0);
}

void esp32s3_apb_saradc_rtc_result(ESP32S3ApbSaradcState *s, unsigned unit,
                                   uint32_t data16, bool int_en)
{
    uint32_t mask = unit ? APB_SARADC_INT_ADC2_DONE : APB_SARADC_INT_ADC1_DONE;

    s->regs[unit ? R_DATA_STATUS2 : R_DATA_STATUS1] = data16 & 0x1ffff;
    if (int_en) {
        s->regs[R_INT_RAW] |= mask;
        esp32s3_apb_saradc_update_irq(s);
    }
}

uint32_t esp32s3_apb_saradc_get_reg(ESP32S3ApbSaradcState *s, unsigned word)
{
    return s->regs[word];
}

static uint64_t esp32s3_apb_saradc_read(void *opaque, hwaddr addr,
                                        unsigned int size)
{
    ESP32S3ApbSaradcState *s = ESP32S3_APB_SARADC(opaque);
    uint32_t idx = addr >> 2;

    switch (addr) {
    case 0x10: /* SAR1_STATUS: DIG FSM idle (DIG controller not modeled) */
    case 0x14: /* SAR2_STATUS */
        return 0;
    case 0x40: /* APB_SARADC1_DATA_STATUS */
        return s->regs[R_DATA_STATUS1];
    case 0x64: /* INT_ST */
        return s->regs[R_INT_RAW] & s->regs[R_INT_ENA];
    case 0x78: /* APB_SARADC2_DATA_STATUS */
        return s->regs[R_DATA_STATUS2];
    case 0x3FC: /* APB_CTRL_DATE */
        return s->regs[R_APB_CTRL_DATE];
    default:
        break;
    }

    if (idx < ARRAY_SIZE(s->regs)) {
        return s->regs[idx];
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "esp32s3.apb_saradc: read out of range 0x%03x\n",
                  (unsigned)addr);
    return 0;
}

static void esp32s3_apb_saradc_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned int size)
{
    ESP32S3ApbSaradcState *s = ESP32S3_APB_SARADC(opaque);
    uint32_t idx = addr >> 2;
    uint32_t prev_start;

    switch (addr) {
    case 0x00: /* CTRL */
        prev_start = s->regs[R_CTRL] & CTRL_START;
        s->regs[R_CTRL] = value;
        /* DIG-controller start (START with START_FORCE or the FSM start the
         * ULP/DIG paths use): the DIG FSM is ADC-03 scope; fail closed
         * without fabricating DONE/data. */
        if (!prev_start && (value & CTRL_START) &&
            (value & CTRL_START_FORCE)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3.apb_saradc: DIG-controller start is not"
                          " modeled (continuous/DMA is ADC-03); no conversion"
                          " runs and no DONE is raised\n");
        }
        return;
    case 0x04: /* CTRL2 */
        prev_start = s->regs[R_CTRL2];
        s->regs[R_CTRL2] = value;
        if (!(prev_start & CTRL2_TIMER_EN) && (value & CTRL2_TIMER_EN)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3.apb_saradc: DIG timer trigger is not"
                          " modeled (ADC-03); stored, no conversions\n");
        }
        return;
    case 0x10: /* SAR1_STATUS RO */
    case 0x14: /* SAR2_STATUS RO */
        return;
    case 0x40: /* DATA_STATUS1 RO */
    case 0x78: /* DATA_STATUS2 RO */
        return;
    case 0x5C: /* INT_ENA */
        s->regs[R_INT_ENA] = value;
        esp32s3_apb_saradc_update_irq(s);
        return;
    case 0x60: /* INT_RAW: W1S (HW events also set bits) */
        s->regs[R_INT_RAW] |= value;
        esp32s3_apb_saradc_update_irq(s);
        return;
    case 0x64: /* INT_ST RO */
        return;
    case 0x68: /* INT_CLR: W1C */
        s->regs[R_INT_RAW] &= ~value;
        esp32s3_apb_saradc_update_irq(s);
        return;
    case 0x6C: /* DMA_CONF */
        if (value & DMA_CONF_RESET_FSM) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3.apb_saradc: APB_ADC_RESET_FSM pulsed (DIG"
                          " FSM reset; ADC-03 scope, no FSM to reset)\n");
        }
        s->regs[R_DMA_CONF] = value & ~DMA_CONF_RESET_FSM;
        return;
    case 0x3FC: /* DATE RO */
        return;
    default:
        break;
    }

    if (idx < ARRAY_SIZE(s->regs)) {
        s->regs[idx] = value;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.apb_saradc: write out of range 0x%03x\n",
                      (unsigned)addr);
    }
}

static const MemoryRegionOps esp32s3_apb_saradc_ops = {
    .read = esp32s3_apb_saradc_read,
    .write = esp32s3_apb_saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void esp32s3_apb_saradc_reset_hold(Object *obj, ResetType type)
{
    ESP32S3ApbSaradcState *s = ESP32S3_APB_SARADC(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_CTRL] = APB_SARADC_CTRL_RESET;
    s->regs[R_CTRL2] = APB_SARADC_CTRL2_RESET;
    s->regs[R_FSM_WAIT] = APB_SARADC_FSM_WAIT_RESET;
    s->regs[R_ARB_CTRL] = APB_SARADC_ARB_RESET;
    s->regs[R_CLKM_CONF] = APB_SARADC_CLKM_RESET;
    s->regs[R_FILTER_CTRL0] = APB_SARADC_FILTER0_RESET;
    s->regs[R_APB_CTRL_DATE] = APB_SARADC_DATE_RESET;
    qemu_set_irq(s->irq, 0);
}

static void esp32s3_apb_saradc_init(Object *obj)
{
    ESP32S3ApbSaradcState *s = ESP32S3_APB_SARADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_apb_saradc_ops, s,
                          TYPE_ESP32S3_APB_SARADC, APB_SARADC_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->apb_clk = qdev_init_clock_in(DEVICE(obj), "apb-clk", NULL, s,
                                    ClockUpdate);
    s->rtc_fast_clk = qdev_init_clock_in(DEVICE(obj), "rtc-fast-clk", NULL, s,
                                         ClockUpdate);
}

static void esp32s3_apb_saradc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_apb_saradc_reset_hold;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo esp32s3_apb_saradc_info = {
    .name          = TYPE_ESP32S3_APB_SARADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3ApbSaradcState),
    .instance_init = esp32s3_apb_saradc_init,
    .class_init    = esp32s3_apb_saradc_class_init,
};

static void esp32s3_apb_saradc_register_types(void)
{
    type_register_static(&esp32s3_apb_saradc_info);
}

type_init(esp32s3_apb_saradc_register_types)
