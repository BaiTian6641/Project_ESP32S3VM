/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ESP32S3_LCD_CAM_H
#define HW_ESP32S3_LCD_CAM_H
#include "hw/sysbus.h"
#include "hw/clock.h"
#include "hw/dma/esp_gdma.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "qemu/timer.h"
#define TYPE_ESP32S3_LCD_CAM "esp32s3-lcd-cam"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3LcdCamState, ESP32S3_LCD_CAM)
#define S3_LCDCAM_FIFO_BYTES 32
#define S3_LCDCAM_REG_WORDS 64
typedef struct S3LcdCamPeriod {
    uint64_t whole, remainder, denominator, fraction;
    int64_t next_ns;
} S3LcdCamPeriod;
struct ESP32S3LcdCamState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Clock *bus, *xtal, *pll160, *pll240;
    ESPGdmaState *gdma;
    ESP32S3GPIOState *gpio;
    DeviceState *electrical, *soc_reset;
    QEMUTimer *lcd_timer, *cam_clock_timer, *cam_filter_timer, *lcd_delay_timer;
    S3LcdCamPeriod lcd_period, cam_period, lcd_module_period;
    int64_t lcd_module_anchor_ns, lcd_drive_deadline[20];
    uint32_t lcd_drive_pending;
    uint8_t lcd_drive_value[20], lcd_drive_mode[20];
    uint32_t reg[S3_LCDCAM_REG_WORDS], lcd_active[S3_LCDCAM_REG_WORDS];
    uint32_t cam_active[4], raw, ena;
    uint8_t tx_fifo[S3_LCDCAM_FIFO_BYTES], rx_fifo[S3_LCDCAM_FIFO_BYTES];
    unsigned tx_head, tx_count, rx_head, rx_count;
    unsigned lcd_phase, phase_cycle, data_cycle, lcd_x, lcd_y;
    unsigned tx_channel, rx_channel, cam_line, cam_segment_bytes, cam_frame_bytes;
    uint16_t lcd_word;
    uint64_t lcd_words, lcd_frames, cam_bytes, cam_frames;
    uint64_t tx_underflow, rx_overflow, invalid_edges, dropped_frames;
    uint64_t last_lcd_eof_ns, last_lcd_done_ns, last_cam_frame_ns;
    int64_t cam_filter_deadline;
    bool tx_eof, tx_list_done, tx_fault, rx_fault;
    bool lcd_running, lcd_clock_level, cam_clock_level;
    bool lcd_pending_update, lcd_stalled, cam_synchronized;
    bool cam_last_clock, cam_last_vsync, cam_last_href, cam_inputs_valid;
    bool cam_filter_candidate, cam_filter_pending;
    bool gate, reset_asserted, notifying, subscribed;
    bool rx_swap_pending;
    uint8_t rx_swap_byte;
};
void esp32s3_lcd_cam_resolved_frame(void *opaque, uint64_t sample_ns);
#endif
