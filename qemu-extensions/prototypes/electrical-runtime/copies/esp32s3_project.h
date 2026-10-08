/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_PROJECT_H
#define ESP32S3_PROJECT_H
#include "qapi/error.h"
#include "qapi/qmp/qobject.h"
#include "qapi/qmp/qdict.h"
#include "net-adapter.h"

#define ESP32S3_PROJECT_MAX_MODELS 32
#define ESP32S3_PROJECT_MAX_MODEL_DRIVES 32
typedef struct Esp32S3ProjectDrive {
    size_t flat_terminal;
    bool oe, level, open_drain;
} Esp32S3ProjectDrive;
typedef struct Esp32S3ProjectDevice {
    size_t component_index;
    EnAdptNativeModel model;
    char *factory_identity; /* Owned Apply-time validated canonical service key. */
    unsigned address;
    size_t sda_terminal, scl_terminal, vdd_terminal, gnd_terminal;
    size_t mosi_terminal, miso_terminal, sclk_terminal, cs_terminal;
    size_t bclk_terminal, ws_terminal, din_terminal, dout_terminal;
    size_t drive_count;
    Esp32S3ProjectDrive drives[ESP32S3_PROJECT_MAX_MODEL_DRIVES];
    /* Set only by successful service-factory preflight for a master peer. */
    bool master_clock_allowed;
    /* Only committed live solver samples may update physical power history. */
    bool power_known, powered;
    uint64_t power_on_ns, power_epoch;
} Esp32S3ProjectDevice;
typedef struct Esp32S3Project {
    QObject *document;
    char *identity;
    bool rc;
    EnRcChargePolicy charge_policy;
    EnAdptGraph graph;
    EnAdptComponent components[EN_ADPT_MAX_COMPONENTS];
    QDict *component_documents[EN_ADPT_MAX_COMPONENTS]; /* Borrowed document records. */
    EnAdptTerminal terminals[EN_ADPT_MAX_TERMINALS];
    EnAdptParameter parameters[EN_ADPT_MAX_COMPONENTS][8];
    EnAdptNet nets[EN_ADPT_MAX_NETS];
    EnAdptId endpoints[EN_ADPT_MAX_TERMINALS];
    size_t terminal_count;
    int pad_terminal[49];
    int vdd_terminal, gnd_terminal;
    EnAdptProjection projection;
    size_t device_count;
    Esp32S3ProjectDevice devices[ESP32S3_PROJECT_MAX_MODELS];
    EnAdptModelBinding models[ESP32S3_PROJECT_MAX_MODELS];
} Esp32S3Project;
Esp32S3Project *esp32s3_project_parse(const char *json, Error **errp);
void esp32s3_project_free(Esp32S3Project *project);
#endif
