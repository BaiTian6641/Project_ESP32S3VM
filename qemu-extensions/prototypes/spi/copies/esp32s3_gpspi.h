/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_ESP32S3_GPSPI_H
#define HW_MISC_ESP32S3_GPSPI_H
#include "hw/sysbus.h"
#include "hw/clock.h"
#include "hw/dma/esp_gdma.h"
#include "qemu/timer.h"
#define TYPE_ESP32S3_GPSPI "esp32s3-gpspi"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3GpSpiState, ESP32S3_GPSPI)
typedef struct S3SPIProvider {
    void *opaque;
    bool (*drive)(void *, unsigned controller, bool clk, bool mosi,
                  bool mosi_oe, unsigned active_cs, unsigned cs_polarity);
    bool (*sample)(void *, unsigned controller, bool *miso);
    void (*end)(void *, unsigned controller, bool abort);
} S3SPIProvider;
typedef enum S3SPIPhase { S3_SPI_SETUP, S3_SPI_COMMAND, S3_SPI_ADDRESS,
    S3_SPI_DUMMY, S3_SPI_TX, S3_SPI_RX, S3_SPI_HOLD, S3_SPI_IDLE } S3SPIPhase;
struct ESP32S3GpSpiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Clock *xtal, *pll80;
    QEMUTimer *timer;
    QEMUBH *pause_bh;
    DeviceState *soc_reset;
    ESPGdmaState *gdma;
    S3SPIProvider provider;
    uint32_t reg[64], active[64];
    uint32_t controller, raw, ena, tx_channel, rx_channel;
    uint32_t bit, bits, data_bits, cs_mask, cs_polarity;
    uint64_t ticks, high_ticks, low_ticks, period_ticks;
    int64_t epoch_ns;
    S3SPIPhase phase;
    uint8_t tx_byte, rx_byte;
    bool gate, reset_asserted, busy, waiting, leading, cpol, cpha, mosi;
    bool tx_dma, rx_dma, tx_lsb, rx_lsb, duplex, tx_enabled, rx_enabled;
};
void esp32s3_gpspi_bind(ESP32S3GpSpiState *, const S3SPIProvider *);
#endif
