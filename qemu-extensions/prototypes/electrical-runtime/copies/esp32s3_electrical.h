/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_ELECTRICAL_H
#define ESP32S3_ELECTRICAL_H
#include "hw/qdev-core.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/misc/esp32s3_rtc_io.h"
#include "hw/misc/esp32s3_sens.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#define TYPE_ESP32S3_ELECTRICAL "esp32s3-electrical"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3ElectricalState, ESP32S3_ELECTRICAL)
DeviceState *esp32s3_electrical_create(Object *soc, ESP32S3GPIOState *gpio,
                                      ESP32S3RtcIoState *rtc, ESP32S3SensState *sens);

typedef enum ESP32S3ElectricalModel {
    ESP32S3_ELECTRICAL_SHT21,
    ESP32S3_ELECTRICAL_24C02,
    ESP32S3_ELECTRICAL_SPI_NOR_1M,
    ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER,
    ESP32S3_ELECTRICAL_I2C_SCRIPTED_MASTER,
    ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3,
    ESP32S3_ELECTRICAL_NEC_ENVELOPE_SOURCE,
    ESP32S3_ELECTRICAL_ST7789_I80,
    ESP32S3_ELECTRICAL_RGB_PANEL,
    ESP32S3_ELECTRICAL_OV2640_DVP
} ESP32S3ElectricalModel;
typedef struct ESP32S3ElectricalI2CEndpoint {
    uint64_t generation;
    uint64_t power_on_ns, power_epoch;
    bool power_known;
    char component_id[65];
    ESP32S3ElectricalModel model;
    unsigned sda_node, scl_node, vdd_node, gnd_node;
    double vdd_v, gnd_v;
    bool powered;
} ESP32S3ElectricalI2CEndpoint;
typedef enum ESP32S3ElectricalRoute {
    ESP32S3_ELECTRICAL_ROUTE_OK,
    ESP32S3_ELECTRICAL_ROUTE_NONE,
    ESP32S3_ELECTRICAL_ROUTE_UNPOWERED,
    ESP32S3_ELECTRICAL_ROUTE_COLLISION,
    ESP32S3_ELECTRICAL_ROUTE_UNKNOWN
} ESP32S3ElectricalRoute;
ESP32S3ElectricalRoute esp32s3_electrical_i2c_resolve(
    DeviceState *dev, unsigned sda_gpio, unsigned scl_gpio, unsigned address,
    uint64_t sample_ns, ESP32S3ElectricalI2CEndpoint *endpoint);
/* Physical registered-model power query only; never grants bus reachability.
 * true means known rails (powered may be false). false with a populated ID
 * and NaN rails means unknown power, NOT a fabricated off/power-cycle event. */
bool esp32s3_electrical_i2c_endpoint(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    ESP32S3ElectricalI2CEndpoint *endpoint);
/* Samples only the current virtual time; false/valid=false is a precise
 * dependency, never a coerced bit. No GPIO input injection API exists. */
bool esp32s3_electrical_line(DeviceState *dev, unsigned gpio, uint64_t sample_ns,
                           double *voltage, bool *valid, bool *level);
/* Native mux-selected controller signals, not resolved pad lines. */
bool esp32s3_electrical_set_matrix_drive(DeviceState *dev, unsigned signal,
                                       bool oe, bool level, bool open_drain);
/* Registered native services alone can request finite SDA/SCL low branches
 * to their own explicitly wired GND; false releases that line high-Z. */
bool esp32s3_electrical_i2c_drive(DeviceState *dev, const char *component_id,
                               bool sda_low, bool scl_low);
/* Fixed 32-registration multicast, deterministic registration order. Both
 * callback forms run AFTER a solved frame has been published, including
 * native controller edges, GPIO mux/IE changes, graph edits and RC samples.
 * Duplicate registration is idempotent; capacity failure returns false.
 * Remove the matching pair before destroying opaque. Removal in a callback
 * takes effect immediately; additions wait until the next published frame.
 * Reentrant drive requests queue the next bounded delta, never recurse. */
bool esp32s3_electrical_add_model_notify(DeviceState *dev,
                                       void (*notify)(void *), void *opaque);
void esp32s3_electrical_remove_model_notify(DeviceState *dev,
                                          void (*notify)(void *), void *opaque);
bool esp32s3_electrical_subscribe(DeviceState *dev,
                                 void (*notify)(void *, uint64_t), void *opaque);
void esp32s3_electrical_unsubscribe(DeviceState *dev,
                                   void (*notify)(void *, uint64_t), void *opaque);

typedef struct ESP32S3ElectricalSpiEndpoint {
    char component_id[65];
    ESP32S3ElectricalModel model;
    uint64_t generation, power_on_ns, power_epoch;
    bool power_known, powered;
    unsigned mosi_node, miso_node, sclk_node, cs_node, vdd_node, gnd_node;
    double vdd_v, gnd_v;
} ESP32S3ElectricalSpiEndpoint;
/* MISO gpio UINT_MAX deliberately imposes no master RX route requirement
 * for TX-only commands; the registered output terminal remains explicit.
 * Physical CS must be known low. Same-net routes precede chip selection. */
ESP32S3ElectricalRoute esp32s3_electrical_spi_resolve(
    DeviceState *dev, unsigned mosi_gpio, unsigned miso_gpio,
    unsigned sclk_gpio, unsigned cs_gpio, uint64_t sample_ns,
    ESP32S3ElectricalSpiEndpoint *endpoint);
ESP32S3ElectricalRoute esp32s3_electrical_spi_endpoint(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    ESP32S3ElectricalSpiEndpoint *endpoint);
/* Enumerates only registered SPI endpoints, zero-based, allocation-free;
 * NONE means end/removed, UNKNOWN preserves unknown-vs-off power. */
ESP32S3ElectricalRoute esp32s3_electrical_spi_endpoint_at(
    DeviceState *dev, unsigned index, uint64_t sample_ns,
    ESP32S3ElectricalSpiEndpoint *endpoint);
/* Actual registered terminal input sampling; no MCU/address surrogate. */
bool esp32s3_electrical_spi_sample(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    bool *mosi_valid, bool *mosi, bool *sclk_valid, bool *sclk,
    bool *cs_valid, bool *cs);
bool esp32s3_electrical_spi_drive(DeviceState *dev, const char *component_id,
                                bool miso_oe, bool miso_level);

typedef struct ESP32S3ElectricalEndpoint {
    char component_id[65];
    ESP32S3ElectricalModel model;
    uint64_t generation, power_on_ns, power_epoch;
    bool power_known, powered;
    double vdd_v, gnd_v;
} ESP32S3ElectricalEndpoint;
ESP32S3ElectricalRoute esp32s3_electrical_model_endpoint(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    ESP32S3ElectricalEndpoint *endpoint);
ESP32S3ElectricalRoute esp32s3_electrical_model_endpoint_at(
    DeviceState *dev, ESP32S3ElectricalModel model, unsigned index,
    uint64_t sample_ns, ESP32S3ElectricalEndpoint *endpoint);
bool esp32s3_electrical_terminal_sample(
    DeviceState *dev, const char *component_id, const char *role,
    uint64_t sample_ns, double *voltage, bool *valid, bool *level);
/* Physical absolute voltage observation is independent of device power.
 * A high-Z unpowered output may be externally pulled to a known voltage.
 * This does not supply a logic level or authorize device consumption. */
bool esp32s3_electrical_terminal_voltage(
    DeviceState *dev, const char *component_id, const char *role,
    uint64_t sample_ns, double *voltage, bool *valid);
/* Only declared native output roles. I2C is low/release, SPI MISO and I2S
 * DOUT/master clocks are finite push-pull to their own explicit rails. */
bool esp32s3_electrical_terminal_drive(
    DeviceState *dev, const char *component_id, const char *role,
    bool oe, bool level);

/* Batch desired outputs before any solved-frame publication. Nesting is
 * bounded; outer end synchronously settles unless inside an observer,
 * where it queues the next delta. Sampling in an uncommitted external
 * batch is unavailable; observer dispatch keeps its existing frame. */
bool esp32s3_electrical_begin_update(DeviceState *dev);
bool esp32s3_electrical_end_update(DeviceState *dev);

/* A registered factory validates the complete actual v3 component before
 * commit. It returns a newly allocated, nonempty canonical identity key
 * (including immutable source-vector identity), freed by the core.
 * Callback must not mutate the current runtime. I2S peer requires this
 * validator; absence rejects Apply rather than accepting a no-op model.
 * Activation runs after commit but BEFORE first publication, with drives
 * queued atomically for the next bounded settlement. Remove before
 * opaque destruction. One factory per exact registered model. */
typedef bool (*ESP32S3ElectricalPreflight)(
    void *opaque, const QDict *component, char **identity, Error **errp);
typedef void (*ESP32S3ElectricalActivate)(void *opaque, DeviceState *dev);
bool esp32s3_electrical_register_factory(
    DeviceState *dev, ESP32S3ElectricalModel model,
    ESP32S3ElectricalPreflight preflight,
    ESP32S3ElectricalActivate activate, void *opaque);
void esp32s3_electrical_unregister_factory(
    DeviceState *dev, ESP32S3ElectricalModel model, void *opaque);
/* Committed-only metadata and canonical factory key, borrowed until next
 * successful Apply. No draft/staging JSON and no second identity algorithm. */
const QDict *esp32s3_electrical_model_component(
    DeviceState *dev, const char *component_id, const char **factory_key);
#endif
