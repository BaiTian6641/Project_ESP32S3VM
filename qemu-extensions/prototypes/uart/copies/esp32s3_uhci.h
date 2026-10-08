/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CHAR_ESP32S3_UHCI_H
#define HW_CHAR_ESP32S3_UHCI_H

#include "hw/sysbus.h"
#include "hw/dma/esp_gdma.h"
#include "qemu/main-loop.h"

#define TYPE_ESP32S3_UHCI "esp32s3.uhci"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3UhciState, ESP32S3_UHCI)

typedef struct S3UhciUart {
    void *opaque;
    bool (*write_byte)(void *opaque, uint8_t byte);
    bool (*read_byte)(void *opaque, uint8_t *byte);
    unsigned (*tx_free)(void *opaque);
    unsigned (*rx_used)(void *opaque);
} S3UhciUart;

typedef enum S3UhciRxEvent {
    S3_UHCI_RX_IDLE,
    S3_UHCI_RX_BREAK,
} S3UhciRxEvent;

/* The largest S3 RX FIFO is 512 bytes (UART0: four 128-byte blocks).
 * One boundary per possible raw FIFO position plus an empty boundary suffices;
 * duplicate physical idle/break events coalesce without hot allocations. */
#define S3_UHCI_RX_BOUNDARIES 513

struct ESP32S3UhciState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ESPGdmaState *gdma;
    DeviceState *soc_reset;
    QEMUBH *bh;
    S3UhciUart uart[3];
    uint32_t reg[0x88 / 4];
    uint32_t rx_channel, rx_length, tx_channel;
    uint64_t rx_raw_sequence, rx_event_end[S3_UHCI_RX_BOUNDARIES];
    bool rx_event_delimiter[S3_UHCI_RX_BOUNDARIES];
    uint16_t rx_event_head, rx_event_count;
    int packet_uart;
    uint8_t tx_pending[4], tx_count, tx_offset;
    uint8_t rx_pending, rx_prefix;
    bool rx_have_pending, rx_have_prefix, rx_started, rx_bound;
    bool tx_open, tx_close, tx_bound, rx_end_pending, rx_error, rx_boundary_pending;
    bool gate, reset_asserted, servicing, notified, diagnostic;
};

/* Attach the actual UART FIFO operations, never a chardev or loopback. */
void esp32s3_uhci_attach_uart(ESP32S3UhciState *s, unsigned uart,
                            const S3UhciUart *ops);
/* Register this with UART FIFO notifications and GDMA UHCI demand notifications. */
void esp32s3_uhci_kick(void *opaque);
/* Metadata is sampled after the UART optionally enqueues its physical frame.
 * delimiter_queued qualifies a real all-low NULL frame only; queued_bytes is
 * the exact UART FIFO count at this event, not a later poll. */
void esp32s3_uhci_rx_event(ESP32S3UhciState *s, unsigned uart,
                         S3UhciRxEvent event, bool delimiter_queued,
                         unsigned queued_bytes);
/* One bounded native-controller quantum; also used by native unit vectors. */
void esp32s3_uhci_service(ESP32S3UhciState *s);

#endif
