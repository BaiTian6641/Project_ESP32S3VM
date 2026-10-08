/*
 * ESP32-S3 SENS (Analog Sensor) controller — RTC ADC measurement state
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Real register model of the SENS block at 0x60008800 (DR_REG_SENS_BASE,
 * ESP32-S3 TRM v1.8 chapter 39; ESP-IDF v6.1.0 components/soc/esp32s3/
 * register/soc/sens_{reg,struct}.h).  The ADC1/ADC2 RTC-controller oneshot
 * measurement path is fully modeled.  Everything else (touch, temperature
 * sensor, ULP/COCPU, SAR2/PWDET) is stored register state with real
 * read/write/W1C semantics but NO fabricated analog results: unsupported
 * measurements fail closed (no READY, no DONE, no data) with a diagnostic.
 * This device never reports an unconditional DONE and never substitutes
 * midscale.  See hw/misc/esp32s3_sens.c for the full behavior contract and
 * include/hw/misc/esp32s3_sens.h for the wiring.
 */
#ifndef HW_MISC_ESP32S3_SENS_H
#define HW_MISC_ESP32S3_SENS_H

#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/clock.h"
#include "qemu/timer.h"
#include "hw/misc/esp32s3_adc_provider.h"
#include "hw/misc/esp32s3_apb_saradc.h"

#define TYPE_ESP32S3_SENS "esp32s3.sens"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3SensState, ESP32S3_SENS)

#define SENS_MEM_SIZE 0x200

/* RTC ADC controller FSM profile (documented encoding, 0 = idle). */
typedef enum Esp32S3SarFsm {
    SAR_FSM_IDLE = 0,
    SAR_FSM_XPD = 1,        /* analog power-up, xpd_wait sar clocks */
    SAR_FSM_SAMPLE = 2,     /* acquisition aperture, 4 * sample_cycle */
    SAR_FSM_CONVERT = 3,    /* 12-bit successive approximation */
    SAR_FSM_RSTB = 4,       /* reference settle, rstb_wait */
    SAR_FSM_STANDBY = 5,    /* power-down tail, standby_wait */
} Esp32S3SarFsm;

typedef struct ESP32S3SarUnit {
    QEMUTimer *timer;
    unsigned index;         /* 0 = ADC1, 1 = ADC2 */
    Esp32S3SarFsm fsm;      /* live phase (SAR_FSM_*) */
    uint8_t channel;        /* channel being measured */
    uint8_t atten;          /* attenuation latched at start */
    uint32_t code;          /* quantized result of the acquisition */
    uint8_t arb_extra;      /* ADC2 arbiter stable cycles */
    bool arbiter_denied;    /* ADC2: grant denied -> flag data */
} ESP32S3SarUnit;

struct ESP32S3SensState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq sar2_armed_irq;    /* named output "sar2-armed" */
    qemu_irq tsens_dump_irq;    /* named output "tsens-dump-out" */

    /* Physical sample source (net solver layer; absent = all UNKNOWN). */
    Esp32S3AdcSampleProvider *provider;

    ESP32S3SarUnit unit[2];

    /* Whole-block register file (word indexed; offsets are SENS + 4*i). */
    uint32_t regs[SENS_MEM_SIZE / 4];

    ESP32S3ApbSaradcState *apb_saradc;  /* link "apb-saradc" (FSM_WAIT, IRQ) */
    DeviceState *regi2c;                /* link "regi2c" (slave-reg storage) */
    bool strict_invalid_sample;         /* stop the VM on wiring errors */
    QEMUBH *pause_bh;                   /* main-loop pause, outside timerlist */
    uint64_t pause_epoch, pause_scheduled_epoch;
    uint64_t pause_source_generation[2];
    uint8_t pause_channel[2], pause_pending;
    bool pause_source_dependent[2];

    /* Clock inputs (CoreClockWorker outputs; constant fallback documented). */
    Clock *apb_clk;
    Clock *rtc_fast_clk;

    /* Diagnostics counters (visible as QOM properties "diag-*"). */
    uint64_t diag_invalid_sample;
    uint64_t diag_floating_sample;
    uint64_t diag_unpowered_sample;
    uint64_t diag_digital_owned_sample;
    uint64_t diag_unknown_sample;
    uint64_t diag_gate_blocked;
    uint64_t diag_unsupported_meas;
    uint64_t diag_arbiter_denied;
    uint64_t diag_ulp_ignored;
    uint64_t diag_dig_force_ignored;
};

/* Profile constants (documented, ideal profile — no silicon calibration). */
#define ESP32S3_ADC_IDEAL_VREF_MV      1100.0   /* nominal internal reference */
#define ESP32S3_ADC_IDEAL_VDD_V        3.3      /* VDD_A rail clamp */

/* Attenuation gains (dB) indexed by SENS_SARn_ATTEN field 0..3. */
extern const double esp32s3_adc_atten_db[4];

/*
 * Ideal voltage-to-code quantization at the modeled reference.  Input is
 * clamped to [0, VDD_A]; the ratio to the attenuation-scaled full scale is
 * rounded to the 12-bit code.  Deliberately linear and offset-free: this is
 * NOT a silicon calibration claim (ADC-02 owns calibrated profiles).
 */
uint32_t esp32s3_adc_ideal_quantize(double voltage_v, unsigned atten);

#endif /* HW_MISC_ESP32S3_SENS_H */
