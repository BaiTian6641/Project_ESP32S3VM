/*
 * ESP32-S3 APB_SARADC (digital ADC controller) register model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Real register model of the APB_SARADC block at 0x60040000
 * (DR_REG_APB_SARADC_BASE, ESP32-S3 TRM v1.8 chapter 39; ESP-IDF v6.1.0
 * components/soc/esp32s3/register/soc/apb_saradc_{reg,struct}.h).
 *
 * Modeled for the oneshot profile (ADC-01):
 *  - full interrupt mapper: INT_RAW (W1S), INT_ENA, INT_ST = raw & ena,
 *    INT_CLR (W1C), level IRQ output for ETS_APB_ADC_INTR_SOURCE;
 *  - ADC1/ADC2 data status mirrors of the latest RTC-controller result,
 *    pushed by the SENS device on completion;
 *  - CTRL / CTRL2 / FSM_WAIT / pattern tables / ARB_CTRL / CLKM_CONF /
 *    DMA_CONF stored with TRM reset defaults; FSM_WAIT and ARB_CTRL are
 *    consumed by the SENS RTC controller (SAR1 waits, ADC2 arbiter);
 *  - DATE register (RO).
 *
 * NOT modeled (separate acceptance packages, extension points only):
 *  - the DIG controller FSM: TIMER_EN / START (+START_FORCE) writes and
 *    DMA enable are stored and diagnosed, they never fabricate conversions
 *    or DONE events (ADC-03 owns continuous/DMA, ADC-04 filters/monitors);
 *  - threshold monitors and IIR filters: thresholds stored, no monitor IRQs.
 */
#ifndef HW_MISC_ESP32S3_APB_SARADC_H
#define HW_MISC_ESP32S3_APB_SARADC_H

#include "hw/sysbus.h"
#include "hw/clock.h"

#define TYPE_ESP32S3_APB_SARADC "esp32s3.apb_saradc"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3ApbSaradcState, ESP32S3_APB_SARADC)

#define APB_SARADC_MEM_SIZE 0x400

/* TRM reset defaults (apb_saradc_reg.h). */
#define APB_SARADC_CTRL_RESET \
    ((1u << 30) | (15u << 19) | (15u << 15) | (4u << 7) | BIT(6))
#define APB_SARADC_CTRL2_RESET  ((10u << 12) | (255u << 1))
#define APB_SARADC_FSM_WAIT_RESET ((255u << 16) | (8u << 8) | 8u)
#define APB_SARADC_ARB_RESET    ((2u << 10) | (1u << 8))
#define APB_SARADC_CLKM_RESET   (4u << 0)
#define APB_SARADC_FILTER0_RESET ((0xdu << 19) | (0xdu << 14))
#define APB_SARADC_DATE_RESET   0x02101180u

/* INT bits (bit31..bit26, shared layout in RAW/ENA/ST/CLR). */
#define APB_SARADC_INT_ADC1_DONE  BIT(31)
#define APB_SARADC_INT_ADC2_DONE  BIT(30)
#define APB_SARADC_INT_THRES_MASK (0x3fu << 26)

struct ESP32S3ApbSaradcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[APB_SARADC_MEM_SIZE / 4];

    Clock *apb_clk;
    Clock *rtc_fast_clk;
};

/*
 * Push an RTC-controller conversion result (called by the SENS device):
 * updates the APB_SARADCn_DATA_STATUS mirror and, when @int_en (SENS
 * SARn_INT_EN), sets the ADC1_DONE/ADC2_DONE INT_RAW bit and the level IRQ.
 */
void esp32s3_apb_saradc_rtc_result(ESP32S3ApbSaradcState *s, unsigned unit,
                                   uint32_t data16, bool int_en);

/* Register-file accessor for the SENS device (word index; FSM_WAIT/ARB). */
uint32_t esp32s3_apb_saradc_get_reg(ESP32S3ApbSaradcState *s, unsigned word);

#endif /* HW_MISC_ESP32S3_APB_SARADC_H */
