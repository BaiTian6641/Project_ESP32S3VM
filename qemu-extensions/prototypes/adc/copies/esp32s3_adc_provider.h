/*
 * ESP32-S3 ADC physical sample provider interface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Contract between the ADC controllers (SENS RTC controller / APB_SARADC) and
 * the analog net layer.  The ADC NEVER invents a voltage: every conversion
 * resolves its acquisition aperture at virtual time by calling the provider.
 * A provider that cannot answer with a known, powered, analog-owned level
 * must say so; the ADC then fails the measurement closed (no DONE, no
 * consumable data, wiring diagnostic) instead of substituting 0 or midscale.
 *
 * Implementers: the net solver layer (parent integration) and test-only QOM
 * providers.  In the integrated tree, validity classification uses the
 * qemu-gpio lane drive snapshots (esp32s3_gpio_get_drive_snapshot() /
 * esp32s3_rtc_io_get_drive_snapshot()) plus net-solver state; the ADC stays a
 * pure consumer of the resolved sample.
 */
#ifndef HW_MISC_ESP32S3_ADC_PROVIDER_H
#define HW_MISC_ESP32S3_ADC_PROVIDER_H

#include "qom/object.h"

#define TYPE_ESP32S3_ADC_SAMPLE_PROVIDER "esp32s3-adc-sample-provider"

typedef enum Esp32S3AdcSampleValidity {
    ESP32S3_ADC_SAMPLE_VALID = 0,    /* known, powered, analog-owned level */
    ESP32S3_ADC_SAMPLE_FLOATING,     /* unresolved net; no valid level */
    ESP32S3_ADC_SAMPLE_UNPOWERED,    /* analog rails down */
    ESP32S3_ADC_SAMPLE_DIGITAL_OWNED, /* pad driven by digital GPIO, not analog */
    ESP32S3_ADC_SAMPLE_UNKNOWN,      /* provider has no answer (wiring error) */
} Esp32S3AdcSampleValidity;

typedef struct Esp32S3AdcSample {
    double voltage_v;               /* meaningful only when validity == VALID */
    Esp32S3AdcSampleValidity validity;
    uint64_t sample_ns;             /* virtual time of the evaluation (echo) */
} Esp32S3AdcSample;

typedef struct Esp32S3AdcSampleProvider Esp32S3AdcSampleProvider;

typedef struct Esp32S3AdcSampleProviderClass {
    InterfaceClass parent_class;

    /*
     * Resolve the level of one ADC channel at virtual time.  @unit is
     * 0 (ADC1) or 1 (ADC2); @channel 0..9 (ADC1 = GPIO1..10, ADC2 =
     * GPIO11..20).  @out->sample_ns must carry the requested instant back so
     * the controller can diagnose time-travel/stale providers.
     */
    void (*sample)(Esp32S3AdcSampleProvider *provider, unsigned unit,
                   unsigned channel, Esp32S3AdcSample *out);

    /* Physical-source revision for this channel. Reading it must not solve
     * a circuit or allocate. A changed revision invalidates a queued wiring
     * pause, not the failed acquisition's no-DONE/data truth. */
    uint64_t (*source_generation)(Esp32S3AdcSampleProvider *provider,
                                  unsigned unit, unsigned channel);
} Esp32S3AdcSampleProviderClass;

#define ESP32S3_ADC_SAMPLE_PROVIDER_CLASS(klass) \
    OBJECT_CLASS_CHECK(Esp32S3AdcSampleProviderClass, (klass), \
                       TYPE_ESP32S3_ADC_SAMPLE_PROVIDER)
#define ESP32S3_ADC_SAMPLE_PROVIDER_GET_CLASS(obj) \
    OBJECT_GET_CLASS(Esp32S3AdcSampleProviderClass, (obj), \
                     TYPE_ESP32S3_ADC_SAMPLE_PROVIDER)

#define ESP32S3_ADC_SAMPLE_PROVIDER(obj) \
    INTERFACE_CHECK(Esp32S3AdcSampleProvider, (obj), \
                    TYPE_ESP32S3_ADC_SAMPLE_PROVIDER)

#endif /* HW_MISC_ESP32S3_ADC_PROVIDER_H */
