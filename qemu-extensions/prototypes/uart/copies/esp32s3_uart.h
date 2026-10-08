/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "esp32_uart.h"
#include "hw/clock.h"
#include "hw/gpio/esp32s3_gpio.h"

#define TYPE_ESP32S3_UART "esp32s3_soc.uart"
#define ESP32S3_UART(obj) OBJECT_CHECK(ESP32S3UARTState, (obj), TYPE_ESP32S3_UART)
#define ESP32S3_UART_CLASS(klass) OBJECT_CLASS_CHECK(ESP32S3UARTClass, klass, TYPE_ESP32S3_UART)
#define ESP32S3_UART_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32S3UARTClass, obj, TYPE_ESP32S3_UART)
#define ESP32S3_UART_REGS 33
#define TYPE_ESP32S3_UART_MEMORY "esp32s3-uart-memory"
typedef struct ESP32S3UARTMemory {
    Object parent_obj;
    uint8_t bytes[1024];
} ESP32S3UARTMemory;

typedef struct ESP32S3UARTState {
    ESP32UARTState parent;
    ESP32S3GPIOState *gpio;
    DeviceState *electrical;
    ESP32S3UARTMemory *fifo_memory;
    Clock *apb, *xtal, *rc_fast;
    uint32_t index;
    uint32_t console_baud;
    uint32_t regs[ESP32S3_UART_REGS];
    uint32_t core[ESP32S3_UART_REGS];
    QEMUTimer tx_timer, rx_timer, timeout_timer, at_timer, console_timer, idle_timer;
    Fifo8 console_rx;
    uint64_t tx_origin, rx_origin, console_origin, bit_num, bit_den;
    uint64_t edge_ns, rise_ns, fall_ns, last_rx_end, frame_rx_start;
    uint16_t tx_frame;
    uint8_t tx_byte, rx_byte, rx_step, at_count;
    uint8_t console_byte, console_step;
    bool console_level;
    uint64_t console_edge_ns;
    uint16_t tx_tick;
    uint16_t tx_next_tick;
    uint8_t tx_wait_phase;
    uint8_t tx_data_bits, rx_data_bits, tx_stop_half, rx_stop_half;
    uint8_t tx_irda_pulse_ticks;
    uint16_t rx_rptr, rx_wptr, tx_rptr, tx_wptr;
    uint16_t rx_capacity, tx_capacity;
    bool tx_active, rx_active, rx_parity, tx_parity, rx_error;
    bool tx_level, rx_level, rx_valid, cts_level, cts_valid, rts_level;
    bool dtr_level;
    bool trace;
    bool gate, held_reset, break_active, console_active, notifying;
    bool rx_all_low, at_pending, finalized;
    bool tx_irda, rx_irda, irda_zero, break_sent;
    bool tx_post_delay;
    bool tx_idle_active;
    bool sampling_idle, idle_waiting;
    bool fifo_shared;
    /* RX input routing signature; a change re-baselines edge detection. */
    uint32_t rx_route;
    uint64_t remaining[6], remaining_num[6], remaining_den[6];
    unsigned rx_next_half;
    void (*dma_notify)(void *opaque);
    void *dma_opaque;
    void (*rx_event)(void *opaque, unsigned index, bool is_break,
                     bool delimiter_queued, unsigned queued_bytes);
    void *rx_event_opaque;
} ESP32S3UARTState;

typedef struct ESP32S3UARTClass {
    ESP32UARTClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
} ESP32S3UARTClass;

/* Bytes enter/leave the same per-controller FIFOs as APB accesses. */
bool esp32s3_uart_dma_write(ESP32S3UARTState *s, uint8_t byte);
bool esp32s3_uart_dma_read(ESP32S3UARTState *s, uint8_t *byte);
unsigned esp32s3_uart_tx_free(ESP32S3UARTState *s);
unsigned esp32s3_uart_rx_used(ESP32S3UARTState *s);
void esp32s3_uart_set_dma_notify(ESP32S3UARTState *s,
                               void (*notify)(void *), void *opaque);
void esp32s3_uart_set_rx_event(ESP32S3UARTState *s,
                             void (*event)(void *, unsigned, bool, bool, unsigned),
                             void *opaque);
void esp32s3_uart_net_changed(void *opaque);
bool esp32s3_uart_bind(ESP32S3UARTState *s, ESP32S3GPIOState *gpio,
                      DeviceState *electrical);
