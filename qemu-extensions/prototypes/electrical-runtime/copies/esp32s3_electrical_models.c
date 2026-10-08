/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/cutils.h"
#include "esp32s3_electrical_internal.h"

static bool is_i2c(const Esp32S3ProjectDevice *model)
{
    return model->model == EN_ADPT_MODEL_SHT21 ||
           model->model == EN_ADPT_MODEL_24C02 ||
           model->model == EN_ADPT_MODEL_OV2640_DVP;
}

static bool is_spi(const Esp32S3ProjectDevice *model)
{
    return model->model == EN_ADPT_MODEL_SPI_NOR_1M;
}

static bool is_i2s(const Esp32S3ProjectDevice *model)
{
    return model->model == EN_ADPT_MODEL_I2S_SAMPLE_PEER;
}

static bool is_registered(const Esp32S3ProjectDevice *model)
{
    return (unsigned)model->model <= EN_ADPT_MODEL_OV2640_DVP;
}

static ESP32S3ElectricalModel public_model(EnAdptNativeModel model)
{
    switch (model) {
    case EN_ADPT_MODEL_SHT21:
        return ESP32S3_ELECTRICAL_SHT21;
    case EN_ADPT_MODEL_24C02:
        return ESP32S3_ELECTRICAL_24C02;
    case EN_ADPT_MODEL_SPI_NOR_1M:
        return ESP32S3_ELECTRICAL_SPI_NOR_1M;
    case EN_ADPT_MODEL_I2S_SAMPLE_PEER:
        return ESP32S3_ELECTRICAL_I2S_SAMPLE_PEER;
    case EN_ADPT_MODEL_I2C_SCRIPTED_MASTER:
        return ESP32S3_ELECTRICAL_I2C_SCRIPTED_MASTER;
    case EN_ADPT_MODEL_WS2812_FUNCTIONAL_3V3:
        return ESP32S3_ELECTRICAL_WS2812_FUNCTIONAL_3V3;
    case EN_ADPT_MODEL_NEC_ENVELOPE_SOURCE:
        return ESP32S3_ELECTRICAL_NEC_ENVELOPE_SOURCE;
    case EN_ADPT_MODEL_ST7789_I80:
        return ESP32S3_ELECTRICAL_ST7789_I80;
    case EN_ADPT_MODEL_RGB_PANEL:
        return ESP32S3_ELECTRICAL_RGB_PANEL;
    case EN_ADPT_MODEL_OV2640_DVP:
        return ESP32S3_ELECTRICAL_OV2640_DVP;
    default:
        g_assert_not_reached();
    }
}

static bool sample_valid(const EnRcSample *sample)
{
    return sample && (sample->status == EN_RC_OK ||
                      sample->status == EN_RC_FLOATING);
}

static ESP32S3ElectricalRoute model_power(
    const Esp32S3Project *project, const Esp32S3ProjectDevice *model,
    const EnRcSample *sample, double *vdd, double *gnd)
{
    *vdd = *gnd = NAN;
    if (!sample_valid(sample)) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    unsigned vdd_node = project->projection.terminal_node[model->vdd_terminal];
    unsigned gnd_node = project->projection.terminal_node[model->gnd_terminal];
    if (sample->floating[vdd_node] || sample->floating[gnd_node] ||
        !isfinite(sample->voltage[vdd_node]) ||
        !isfinite(sample->voltage[gnd_node])) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    *vdd = sample->voltage[vdd_node];
    *gnd = sample->voltage[gnd_node];
    double supply = *vdd - *gnd;
    double minimum = model->model == EN_ADPT_MODEL_SHT21 ? 2.1 :
                     model->model == EN_ADPT_MODEL_24C02 ? 2.5 : 2.7;
    double maximum = model->model == EN_ADPT_MODEL_24C02 ? 5.5 : 3.6;
    return supply >= minimum && supply <= maximum ?
           ESP32S3_ELECTRICAL_ROUTE_OK : ESP32S3_ELECTRICAL_ROUTE_UNPOWERED;
}

void esp32s3_electrical_models_update_power(Esp32S3Project *project,
                                           const EnRcSample *sample, uint64_t ns)
{
    if (!project) {
        return;
    }
    for (size_t i = 0; i < project->device_count; ++i) {
        Esp32S3ProjectDevice *model = &project->devices[i];
        double vdd, gnd;
        ESP32S3ElectricalRoute power = model_power(project, model, sample,
                                                  &vdd, &gnd);
        bool known = power != ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
        bool powered = power == ESP32S3_ELECTRICAL_ROUTE_OK;
        if (powered && (!model->power_known || !model->powered)) {
            model->power_on_ns = ns;
            ++model->power_epoch;
        }
        model->power_known = known;
        model->powered = powered;
    }
}

static bool same_terminal(const Esp32S3Project *previous, size_t old_terminal,
                          const Esp32S3Project *candidate, size_t new_terminal)
{
    return old_terminal < previous->terminal_count &&
           new_terminal < candidate->terminal_count &&
           !strcmp(previous->terminals[old_terminal].id.text,
                   candidate->terminals[new_terminal].id.text);
}

void esp32s3_electrical_models_transfer(const Esp32S3Project *previous,
                                       Esp32S3Project *candidate)
{
    if (!previous || !candidate || previous == candidate) {
        return;
    }
    for (size_t i = 0; i < candidate->device_count; ++i) {
        Esp32S3ProjectDevice *next = &candidate->devices[i];
        const char *id = candidate->components[next->component_index].id.text;
        for (size_t j = 0; j < previous->device_count; ++j) {
            const Esp32S3ProjectDevice *old = &previous->devices[j];
            if (next->model != old->model ||
                strcmp(id, previous->components[old->component_index].id.text)) {
                continue;
            }
            next->power_known = old->power_known;
            next->powered = old->powered;
            next->power_on_ns = old->power_on_ns;
            next->power_epoch = old->power_epoch;
            for (size_t ni = 0; ni < next->drive_count; ++ni) {
                Esp32S3ProjectDrive *drive = &next->drives[ni];
                EnAdptTerminalDirection direction =
                    candidate->terminals[drive->flat_terminal].direction;
                if (direction != EN_ADPT_DIRECTION_OUTPUT &&
                    direction != EN_ADPT_DIRECTION_INOUT) {
                    continue;
                }
                if (is_i2s(next) && !next->master_clock_allowed &&
                    (drive->flat_terminal == next->bclk_terminal ||
                     drive->flat_terminal == next->ws_terminal)) {
                    continue;
                }
                for (size_t oi = 0; oi < old->drive_count; ++oi) {
                    const Esp32S3ProjectDrive *source = &old->drives[oi];
                    if (source->open_drain != drive->open_drain ||
                        !same_terminal(previous, source->flat_terminal,
                                       candidate, drive->flat_terminal)) {
                        continue;
                    }
                    bool high = !source->open_drain && source->level;
                    if (same_terminal(previous, high ? old->vdd_terminal : old->gnd_terminal,
                                      candidate, high ? next->vdd_terminal : next->gnd_terminal)) {
                        drive->oe = source->oe;
                        drive->level = source->level;
                    }
                    break;
                }
            }
            break;
        }
    }
}

static Esp32S3ProjectDevice *find_registered_model(Esp32S3Project *project,
                                                 const char *id)
{
    if (!project || !id) {
        return NULL;
    }
    for (size_t i = 0; i < project->device_count; ++i) {
        Esp32S3ProjectDevice *model = &project->devices[i];
        if (is_registered(model) &&
            !strcmp(id, project->components[model->component_index].id.text)) {
            return model;
        }
    }
    return NULL;
}

static Esp32S3ProjectDrive *find_drive(Esp32S3ProjectDevice *model, size_t terminal)
{
    for (size_t i = 0; i < model->drive_count; ++i) {
        if (model->drives[i].flat_terminal == terminal) {
            return &model->drives[i];
        }
    }
    return NULL;
}

static bool request_drive(DeviceState *dev, Esp32S3ProjectDrive *drive,
                          bool oe, bool level)
{
    if (!drive) {
        return false;
    }
    if (drive->open_drain) {
        oe = oe && !level;
        level = false;
    }
    if (drive->oe != oe || drive->level != level) {
        drive->oe = oe;
        drive->level = level;
        return esp32s3_electrical_model_changed(dev);
    }
    return true;
}

bool esp32s3_electrical_i2c_drive(DeviceState *dev, const char *component_id,
                               bool sda_low, bool scl_low)
{
    if (!dev) {
        return false;
    }
    Esp32S3ProjectDevice *model = find_registered_model(esp32s3_electrical_project(dev),
                                                       component_id);
    if (!model || !is_i2c(model)) {
        return false;
    }
    Esp32S3ProjectDrive *sda = find_drive(model, model->sda_terminal);
    Esp32S3ProjectDrive *scl = find_drive(model, model->scl_terminal);
    if (!sda || !scl) {
        return false;
    }
    if (sda->oe != sda_low || scl->oe != scl_low) {
        sda->oe = sda_low;
        scl->oe = scl_low;
        sda->level = scl->level = false;
        return esp32s3_electrical_model_changed(dev);
    }
    return true;
}

bool esp32s3_electrical_spi_drive(DeviceState *dev, const char *component_id,
                                bool miso_oe, bool miso_level)
{
    if (!dev) {
        return false;
    }
    Esp32S3ProjectDevice *model = find_registered_model(esp32s3_electrical_project(dev),
                                                       component_id);
    if (!model || !is_spi(model)) {
        return false;
    }
    return request_drive(dev, find_drive(model, model->miso_terminal),
                         miso_oe, miso_level);
}

static ESP32S3ElectricalRoute i2c_details(
    DeviceState *dev, const Esp32S3Project *project,
    const Esp32S3ProjectDevice *model, const EnRcSample *sample,
    ESP32S3ElectricalI2CEndpoint *endpoint)
{
    endpoint->generation = esp32s3_electrical_generation(dev);
    endpoint->power_on_ns = model->power_on_ns;
    endpoint->power_epoch = model->power_epoch;
    pstrcpy(endpoint->component_id, sizeof(endpoint->component_id),
            project->components[model->component_index].id.text);
    endpoint->model = public_model(model->model);
    endpoint->sda_node = project->projection.terminal_node[model->sda_terminal];
    endpoint->scl_node = project->projection.terminal_node[model->scl_terminal];
    endpoint->vdd_node = project->projection.terminal_node[model->vdd_terminal];
    endpoint->gnd_node = project->projection.terminal_node[model->gnd_terminal];
    ESP32S3ElectricalRoute power = model_power(project, model, sample,
                                               &endpoint->vdd_v, &endpoint->gnd_v);
    endpoint->power_known = power != ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    endpoint->powered = power == ESP32S3_ELECTRICAL_ROUTE_OK;
    return power;
}

static ESP32S3ElectricalRoute spi_details(
    DeviceState *dev, const Esp32S3Project *project,
    const Esp32S3ProjectDevice *model, const EnRcSample *sample,
    ESP32S3ElectricalSpiEndpoint *endpoint)
{
    endpoint->generation = esp32s3_electrical_generation(dev);
    endpoint->power_on_ns = model->power_on_ns;
    endpoint->power_epoch = model->power_epoch;
    pstrcpy(endpoint->component_id, sizeof(endpoint->component_id),
            project->components[model->component_index].id.text);
    endpoint->model = public_model(model->model);
    endpoint->mosi_node = project->projection.terminal_node[model->mosi_terminal];
    endpoint->miso_node = project->projection.terminal_node[model->miso_terminal];
    endpoint->sclk_node = project->projection.terminal_node[model->sclk_terminal];
    endpoint->cs_node = project->projection.terminal_node[model->cs_terminal];
    endpoint->vdd_node = project->projection.terminal_node[model->vdd_terminal];
    endpoint->gnd_node = project->projection.terminal_node[model->gnd_terminal];
    ESP32S3ElectricalRoute power = model_power(project, model, sample,
                                               &endpoint->vdd_v, &endpoint->gnd_v);
    endpoint->power_known = power != ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    endpoint->powered = power == ESP32S3_ELECTRICAL_ROUTE_OK;
    return power;
}

bool esp32s3_electrical_i2c_endpoint(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    ESP32S3ElectricalI2CEndpoint *endpoint)
{
    if (!endpoint) {
        return false;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->vdd_v = endpoint->gnd_v = NAN;
    if (!dev || !component_id) {
        return false;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    Esp32S3ProjectDevice *model = find_registered_model(project, component_id);
    endpoint->generation = esp32s3_electrical_generation(dev);
    return model && is_i2c(model) &&
           i2c_details(dev, project, model, sample, endpoint) !=
           ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
}

ESP32S3ElectricalRoute esp32s3_electrical_spi_endpoint(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    ESP32S3ElectricalSpiEndpoint *endpoint)
{
    if (!endpoint) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->vdd_v = endpoint->gnd_v = NAN;
    if (!dev || !component_id) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    Esp32S3ProjectDevice *model = find_registered_model(project, component_id);
    endpoint->generation = esp32s3_electrical_generation(dev);
    return model && is_spi(model) ?
           spi_details(dev, project, model, sample, endpoint) :
           ESP32S3_ELECTRICAL_ROUTE_NONE;
}

ESP32S3ElectricalRoute esp32s3_electrical_spi_endpoint_at(
    DeviceState *dev, unsigned index, uint64_t sample_ns,
    ESP32S3ElectricalSpiEndpoint *endpoint)
{
    if (!endpoint) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->vdd_v = endpoint->gnd_v = NAN;
    if (!dev) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    endpoint->generation = esp32s3_electrical_generation(dev);
    if (!project) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    for (size_t i = 0; i < project->device_count; ++i) {
        Esp32S3ProjectDevice *model = &project->devices[i];
        if (is_spi(model)) {
            if (!index) {
                return spi_details(dev, project, model, sample, endpoint);
            }
            --index;
        }
    }
    return ESP32S3_ELECTRICAL_ROUTE_NONE;
}

static bool gpio_node(const Esp32S3Project *project, unsigned gpio,
                      unsigned *node)
{
    if (gpio >= G_N_ELEMENTS(project->pad_terminal) ||
        project->pad_terminal[gpio] < 0) {
        return false;
    }
    *node = project->projection.terminal_node[project->pad_terminal[gpio]];
    return *node != UINT_MAX;
}

ESP32S3ElectricalRoute esp32s3_electrical_i2c_resolve(
    DeviceState *dev, unsigned sda_gpio, unsigned scl_gpio, unsigned address,
    uint64_t sample_ns, ESP32S3ElectricalI2CEndpoint *endpoint)
{
    if (!endpoint) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->vdd_v = endpoint->gnd_v = NAN;
    if (!dev) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    endpoint->generation = esp32s3_electrical_generation(dev);
    if (!project) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    unsigned sda, scl;
    if (sda_gpio == scl_gpio || !gpio_node(project, sda_gpio, &sda) ||
        !gpio_node(project, scl_gpio, &scl)) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    size_t active = SIZE_MAX, unknown = SIZE_MAX, off = SIZE_MAX;
    for (size_t i = 0; i < project->device_count; ++i) {
        Esp32S3ProjectDevice *model = &project->devices[i];
        /* Physical same-net routes precede address decode; metadata cannot
         * connect private terminals or grant an ACK through an ID query. */
        if (!is_i2c(model) ||
            sda != project->projection.terminal_node[model->sda_terminal] ||
            scl != project->projection.terminal_node[model->scl_terminal]) {
            continue;
        }
        if (address != model->address) {
            continue;
        }
        double vdd, gnd;
        ESP32S3ElectricalRoute power = model_power(project, model, sample,
                                                   &vdd, &gnd);
        if (power == ESP32S3_ELECTRICAL_ROUTE_OK) {
            if (active != SIZE_MAX) {
                return ESP32S3_ELECTRICAL_ROUTE_COLLISION;
            }
            active = i;
        } else if (power == ESP32S3_ELECTRICAL_ROUTE_UNKNOWN) {
            unknown = i;
        } else {
            off = i;
        }
    }
    size_t found = unknown != SIZE_MAX ? unknown :
                   active != SIZE_MAX ? active : off;
    return found == SIZE_MAX ? ESP32S3_ELECTRICAL_ROUTE_NONE :
           i2c_details(dev, project, &project->devices[found], sample, endpoint);
}

/* Call only after known-powered rails have been obtained from this sample. */
static EnPadLevel terminal_level(const Esp32S3Project *project,
                                const EnRcSample *sample, size_t terminal,
                                double vdd, double gnd)
{
    unsigned node = project->projection.terminal_node[terminal];
    return en_dc_classify_pad(sample->voltage[node] - gnd,
                              sample->floating[node], vdd - gnd, 0.25, 0.75);
}

ESP32S3ElectricalRoute esp32s3_electrical_spi_resolve(
    DeviceState *dev, unsigned mosi_gpio, unsigned miso_gpio,
    unsigned sclk_gpio, unsigned cs_gpio, uint64_t sample_ns,
    ESP32S3ElectricalSpiEndpoint *endpoint)
{
    if (!endpoint) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->vdd_v = endpoint->gnd_v = NAN;
    if (!dev) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    endpoint->generation = esp32s3_electrical_generation(dev);
    if (!project) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    unsigned mosi, miso = UINT_MAX, sclk, cs;
    if (!gpio_node(project, mosi_gpio, &mosi) ||
        !gpio_node(project, sclk_gpio, &sclk) ||
        !gpio_node(project, cs_gpio, &cs) ||
        (miso_gpio != UINT_MAX && !gpio_node(project, miso_gpio, &miso))) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    size_t active = SIZE_MAX, unknown = SIZE_MAX, off = SIZE_MAX;
    for (size_t i = 0; i < project->device_count; ++i) {
        Esp32S3ProjectDevice *model = &project->devices[i];
        /* UINT_MAX skips only master RX reachability. The registered MISO
         * terminal and its finite branch remain part of the actual graph. */
        if (!is_spi(model) ||
            mosi != project->projection.terminal_node[model->mosi_terminal] ||
            sclk != project->projection.terminal_node[model->sclk_terminal] ||
            cs != project->projection.terminal_node[model->cs_terminal] ||
            (miso_gpio != UINT_MAX &&
             miso != project->projection.terminal_node[model->miso_terminal])) {
            continue;
        }
        double vdd, gnd;
        ESP32S3ElectricalRoute power = model_power(project, model, sample,
                                                   &vdd, &gnd);
        if (power == ESP32S3_ELECTRICAL_ROUTE_UNKNOWN) {
            unknown = i;
            continue;
        }
        if (power == ESP32S3_ELECTRICAL_ROUTE_UNPOWERED) {
            off = i;
            continue;
        }
        EnPadLevel selected = terminal_level(project, sample,
                                             model->cs_terminal, vdd, gnd);
        if (selected == EN_PAD_HIGH) {
            continue;
        }
        if (selected != EN_PAD_LOW) {
            unknown = i;
            continue;
        }
        if (active != SIZE_MAX) {
            return ESP32S3_ELECTRICAL_ROUTE_COLLISION;
        }
        active = i;
    }
    size_t found = unknown != SIZE_MAX ? unknown :
                   active != SIZE_MAX ? active : off;
    if (found == SIZE_MAX) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    ESP32S3ElectricalRoute result = spi_details(dev, project,
        &project->devices[found], sample, endpoint);
    return unknown != SIZE_MAX ? ESP32S3_ELECTRICAL_ROUTE_UNKNOWN : result;
}

bool esp32s3_electrical_spi_sample(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    bool *mosi_valid, bool *mosi, bool *sclk_valid, bool *sclk,
    bool *cs_valid, bool *cs)
{
    if (!mosi_valid || !mosi || !sclk_valid || !sclk || !cs_valid || !cs) {
        return false;
    }
    *mosi_valid = *sclk_valid = *cs_valid = false;
    *mosi = *sclk = *cs = false;
    if (!dev || !component_id) {
        return false;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    Esp32S3ProjectDevice *model = find_registered_model(project, component_id);
    if (!model || !is_spi(model) || !sample_valid(sample)) {
        return false;
    }
    double vdd, gnd;
    if (model_power(project, model, sample, &vdd, &gnd) !=
        ESP32S3_ELECTRICAL_ROUTE_OK) {
        return true;
    }
    EnPadLevel ml = terminal_level(project, sample, model->mosi_terminal, vdd, gnd);
    EnPadLevel sl = terminal_level(project, sample, model->sclk_terminal, vdd, gnd);
    EnPadLevel cl = terminal_level(project, sample, model->cs_terminal, vdd, gnd);
    *mosi_valid = ml == EN_PAD_LOW || ml == EN_PAD_HIGH;
    *sclk_valid = sl == EN_PAD_LOW || sl == EN_PAD_HIGH;
    *cs_valid = cl == EN_PAD_LOW || cl == EN_PAD_HIGH;
    *mosi = ml == EN_PAD_HIGH;
    *sclk = sl == EN_PAD_HIGH;
    *cs = cl == EN_PAD_HIGH;
    return true;
}

static size_t registered_terminal(const Esp32S3Project *project,
                                  const Esp32S3ProjectDevice *model,
                                  const char *role)
{
    const EnAdptComponent *component = &project->components[model->component_index];
    for (size_t i = 0; role && i < component->terminal_count; ++i) {
        if (!strcmp(role, component->terminals[i].role)) {
            return component->terminals + i - project->terminals;
        }
    }
    return SIZE_MAX;
}

bool esp32s3_electrical_terminal_voltage(
    DeviceState *dev, const char *component_id, const char *role,
    uint64_t sample_ns, double *voltage, bool *valid)
{
    if (!voltage || !valid) {
        return false;
    }
    *voltage = NAN;
    *valid = false;
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    Esp32S3ProjectDevice *model = find_registered_model(project, component_id);
    if (!model || !role || !sample_valid(sample)) {
        return false;
    }
    size_t terminal = registered_terminal(project, model, role);
    if (terminal == SIZE_MAX) {
        return false;
    }
    unsigned node = project->projection.terminal_node[terminal];
    *voltage = sample->voltage[node];
    *valid = !sample->floating[node] && isfinite(*voltage);
    return true;
}

bool esp32s3_electrical_terminal_sample(
    DeviceState *dev, const char *component_id, const char *role,
    uint64_t sample_ns, double *voltage, bool *valid, bool *level)
{
    if (!voltage || !valid || !level) {
        return false;
    }
    *voltage = NAN;
    *valid = *level = false;
    if (!dev || !component_id || !role) {
        return false;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    Esp32S3ProjectDevice *model = find_registered_model(project, component_id);
    if (!model || !sample_valid(sample)) {
        return false;
    }
    size_t terminal = registered_terminal(project, model, role);
    if (terminal == SIZE_MAX) {
        return false;
    }
    unsigned node = project->projection.terminal_node[terminal];
    *voltage = sample->voltage[node];
    double vdd, gnd;
    if (model_power(project, model, sample, &vdd, &gnd) ==
        ESP32S3_ELECTRICAL_ROUTE_OK) {
        EnPadLevel interpreted = terminal_level(project, sample, terminal, vdd, gnd);
        *valid = interpreted == EN_PAD_LOW || interpreted == EN_PAD_HIGH;
        *level = interpreted == EN_PAD_HIGH;
    }
    return isfinite(*voltage);
}

bool esp32s3_electrical_terminal_drive(
    DeviceState *dev, const char *component_id, const char *role,
    bool oe, bool level)
{
    if (!dev || !component_id || !role) {
        return false;
    }
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    Esp32S3ProjectDevice *model = find_registered_model(project, component_id);
    if (!model) {
        return false;
    }
    size_t terminal = registered_terminal(project, model, role);
    if (terminal == SIZE_MAX) {
        return false;
    }
    Esp32S3ProjectDrive *drive = find_drive(model, terminal);
    if (!drive) {
        return false;
    }
    if (oe) {
        EnAdptTerminalDirection direction = project->terminals[terminal].direction;
        if (direction != EN_ADPT_DIRECTION_OUTPUT &&
            direction != EN_ADPT_DIRECTION_INOUT) {
            return false;
        }
        if (is_i2s(model) && !model->master_clock_allowed &&
            (terminal == model->bclk_terminal || terminal == model->ws_terminal)) {
            return false;
        }
    }
    return request_drive(dev, drive, oe, level);
}

static ESP32S3ElectricalRoute model_details(
    DeviceState *dev, const Esp32S3Project *project,
    const Esp32S3ProjectDevice *model, const EnRcSample *sample,
    ESP32S3ElectricalEndpoint *endpoint)
{
    pstrcpy(endpoint->component_id, sizeof(endpoint->component_id),
            project->components[model->component_index].id.text);
    endpoint->model = public_model(model->model);
    endpoint->generation = esp32s3_electrical_generation(dev);
    endpoint->power_on_ns = model->power_on_ns;
    endpoint->power_epoch = model->power_epoch;
    ESP32S3ElectricalRoute power = model_power(project, model, sample,
                                               &endpoint->vdd_v, &endpoint->gnd_v);
    endpoint->power_known = power != ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    endpoint->powered = power == ESP32S3_ELECTRICAL_ROUTE_OK;
    return power;
}

ESP32S3ElectricalRoute esp32s3_electrical_model_endpoint(
    DeviceState *dev, const char *component_id, uint64_t sample_ns,
    ESP32S3ElectricalEndpoint *endpoint)
{
    if (!endpoint) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->vdd_v = endpoint->gnd_v = NAN;
    if (!dev || !component_id) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    Esp32S3ProjectDevice *model = find_registered_model(project, component_id);
    endpoint->generation = esp32s3_electrical_generation(dev);
    return model ? model_details(dev, project, model, sample, endpoint) :
                   ESP32S3_ELECTRICAL_ROUTE_NONE;
}

ESP32S3ElectricalRoute esp32s3_electrical_model_endpoint_at(
    DeviceState *dev, ESP32S3ElectricalModel model, unsigned index,
    uint64_t sample_ns, ESP32S3ElectricalEndpoint *endpoint)
{
    if (!endpoint) {
        return ESP32S3_ELECTRICAL_ROUTE_UNKNOWN;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->vdd_v = endpoint->gnd_v = NAN;
    if (!dev || (unsigned)model > ESP32S3_ELECTRICAL_OV2640_DVP) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    const EnRcSample *sample = esp32s3_electrical_sample_at(dev, sample_ns);
    Esp32S3Project *project = esp32s3_electrical_project(dev);
    endpoint->generation = esp32s3_electrical_generation(dev);
    if (!project) {
        return ESP32S3_ELECTRICAL_ROUTE_NONE;
    }
    for (size_t i = 0; i < project->device_count; ++i) {
        const Esp32S3ProjectDevice *candidate = &project->devices[i];
        if (is_registered(candidate) &&
            public_model(candidate->model) == model) {
            if (!index) {
                return model_details(dev, project, candidate, sample, endpoint);
            }
            --index;
        }
    }
    return ESP32S3_ELECTRICAL_ROUTE_NONE;
}
