/* SPDX-License-Identifier: Apache-2.0
 * Only public IDF drivers operate hardware. External v3 graph owns all wires.
 * Exact assertions cover every closed pulse, not an unspecified idle trailer.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CAPACITY 192
#define WAIT_MS 100
#define IDLE_NS 1000000
static unsigned failures;
static rmt_encoder_handle_t encoder;
static rmt_symbol_word_t payload[121];
_Alignas(64) static rmt_symbol_word_t receive_storage[2][192];
#if CONFIG_RMT_NATIVE_PROFILE_CONNECTED
static const bool connected = true;
#else
static const bool connected = false;
#endif

typedef struct {
    rmt_channel_handle_t tx, rx;
    volatile unsigned tx_events, rx_events, partial_events;
    volatile size_t count, tx_symbols;
    volatile bool overflow, last, rx_pending;
    volatile int64_t tx_done_us, rx_done_us;
    rmt_symbol_word_t captured[CAPACITY];
    unsigned index;
    uint32_t resolution;
    bool dma;
} lane_t;
static lane_t lanes[2];

static void check(const char *phase, const char *name, bool ok)
{
    printf("RMT_NATIVE_CHECK phase=%s name=%s result=%s\n", phase, name, ok ? "PASS" : "FAIL");
    failures += !ok;
    fflush(stdout);
}

static bool status(const char *phase, const char *name, esp_err_t err)
{
    printf("RMT_NATIVE_STATUS phase=%s name=%s actual=%s expected=ESP_OK\n", phase, name, esp_err_to_name(err));
    check(phase, name, err == ESP_OK);
    return err == ESP_OK;
}

static bool tx_done(rmt_channel_handle_t channel, const rmt_tx_done_event_data_t *event, void *opaque)
{
    (void)channel;
    lane_t *lane = opaque;
    lane->tx_symbols = event->num_symbols;
    lane->tx_done_us = esp_timer_get_time();
    lane->tx_events++;
    return false;
}

static bool rx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *event, void *opaque)
{
    (void)channel;
    lane_t *lane = opaque;
    size_t offset = lane->count;
    if (event->num_symbols > CAPACITY - offset) {
        lane->overflow = true;
    } else {
        memcpy(lane->captured + offset, event->received_symbols,
               event->num_symbols * sizeof(rmt_symbol_word_t));
        lane->count = offset + event->num_symbols;
    }
    lane->rx_events++;
    if (event->flags.is_last) {
        lane->last = true;
        lane->rx_pending = false;
        lane->rx_done_us = esp_timer_get_time();
    } else {
        lane->partial_events++;
    }
    return false;
}

static void reset_observations(lane_t *lane)
{
    lane->tx_events = lane->rx_events = lane->partial_events = 0;
    lane->count = lane->tx_symbols = 0;
    lane->overflow = lane->last = false;
    lane->tx_done_us = lane->rx_done_us = 0;
    memset(lane->captured, 0, sizeof(lane->captured));
}

static bool open_lane(lane_t *lane, unsigned index, rmt_clock_source_t source,
                      uint32_t resolution, bool dma, const char *phase)
{
    memset(lane, 0, sizeof(*lane));
    lane->index = index;
    lane->resolution = resolution;
    lane->dma = dma;
    rmt_tx_channel_config_t tx = {
        .gpio_num = index ? 6 : 4, .clk_src = source,
        .resolution_hz = resolution, .mem_block_symbols = dma ? 192 : 48, .trans_queue_depth = 2,
        .flags.with_dma = dma,
    };
    rmt_rx_channel_config_t rx = {
        .gpio_num = index ? 7 : 5, .clk_src = source,
        .resolution_hz = resolution, .mem_block_symbols = dma ? 192 : 48,
        .flags.with_dma = dma,
    };
    if (!status(phase, "new_tx", rmt_new_tx_channel(&tx, &lane->tx))) return false;
    if (!status(phase, "new_rx", rmt_new_rx_channel(&rx, &lane->rx))) return false;
    rmt_tx_event_callbacks_t tx_cbs = {.on_trans_done = tx_done};
    rmt_rx_event_callbacks_t rx_cbs = {.on_recv_done = rx_done};
    return status(phase, "tx_callback", rmt_tx_register_event_callbacks(lane->tx, &tx_cbs, lane)) &&
           status(phase, "rx_callback", rmt_rx_register_event_callbacks(lane->rx, &rx_cbs, lane)) &&
           status(phase, "enable_tx", rmt_enable(lane->tx)) &&
           status(phase, "enable_rx", rmt_enable(lane->rx));
}

static void close_lane(lane_t *lane, const char *phase)
{
    if (lane->rx) {
        status(phase, "disable_rx", rmt_disable(lane->rx));
        status(phase, "delete_rx", rmt_del_channel(lane->rx));
    }
    if (lane->tx) {
        status(phase, "disable_tx", rmt_disable(lane->tx));
        status(phase, "delete_tx", rmt_del_channel(lane->tx));
    }
    lane->tx = lane->rx = NULL;
}

static bool arm(lane_t *lane, uint32_t min_ns, uint32_t max_ns, const char *phase)
{
    /* A receive that produced no completion (e.g. a quiet disconnected line,
     * where the hardware idle counter starts only at the first edge) leaves
     * the driver fsm in RUN; the next rmt_receive then fails the ENABLE->WAIT
     * CAS. Recover exactly as an ordinary application must: disable/enable. */
    if (lane->rx_pending) {
        if (!status(phase, "recover_rx_disable", rmt_disable(lane->rx)) ||
            !status(phase, "recover_rx_enable", rmt_enable(lane->rx))) {
            return false;
        }
        lane->rx_pending = false;
    }
    reset_observations(lane);
    rmt_receive_config_t cfg = {
        .signal_range_min_ns = min_ns, .signal_range_max_ns = max_ns,
        .flags.en_partial_rx = true,
    };
    esp_err_t armed = rmt_receive(lane->rx, receive_storage[lane->index],
                                  (lane->dma ? 192 : 48) * sizeof(rmt_symbol_word_t), &cfg);
    lane->rx_pending = armed == ESP_OK;
    return status(phase, "receive", armed);
}

static void wait_receive(lane_t *lane)
{
    int64_t deadline = esp_timer_get_time() + WAIT_MS * 1000;
    while (!lane->last && esp_timer_get_time() < deadline) vTaskDelay(1);
}

static void dump(lane_t *lane, const char *phase, int64_t submit_us)
{
    printf("RMT_NATIVE_TIME phase=%s lane=%u submit_us=%" PRId64 " tx_done_us=%" PRId64
           " rx_done_us=%" PRId64 " tx_events=%u rx_events=%u partial_events=%u tx_symbols=%zu rx_symbols=%zu\n",
           phase, lane->index, submit_us, lane->tx_done_us, lane->rx_done_us,
           lane->tx_events, lane->rx_events, lane->partial_events, lane->tx_symbols, lane->count);
    for (size_t i = 0; i < lane->count; ++i) {
        rmt_symbol_word_t s = lane->captured[i];
        printf("RMT_NATIVE_SYMBOL phase=%s lane=%u index=%zu l0=%u d0=%u l1=%u d1=%u\n",
               phase, lane->index, i, s.level0, s.duration0, s.level1, s.duration1);
    }
    fflush(stdout);
}

static uint64_t vector(unsigned count, uint32_t resolution, const char *phase)
{
    uint64_t ticks = 0;
    for (unsigned i = 0; i < count; ++i) {
        payload[i] = (rmt_symbol_word_t){.level0 = 1, .duration0 = 31 + (i * 17) % 71,
                                     .level1 = 0, .duration1 = 43 + (i * 13) % 83};
        printf("RMT_NATIVE_EXPECT phase=%s index=%u l0=1 d0=%u l1=0 d1=%u edge0_ns=%" PRIu64 "\n",
               phase, i, payload[i].duration0, payload[i].duration1,
               ticks * UINT64_C(1000000000) / resolution);
        ticks += payload[i].duration0 + payload[i].duration1;
    }
    /* Guard pulse closes the final payload low half. Its idle low trailer is
     * printed verbatim, never rewritten to the expected payload duration. */
    payload[count] = (rmt_symbol_word_t){.level0 = 1, .duration0 = 37, .level1 = 0, .duration1 = 11};
    return ticks + 48;
}

static void verify_capture(lane_t *lane, const char *phase, unsigned count)
{
    if (!connected) {
        check(phase, "disconnected_no_payload", lane->count == 0 && lane->rx_events == 0 && !lane->last);
        return;
    }
    check(phase, "rx_idle_completion", lane->last && !lane->overflow);
    check(phase, "rx_exact_count", lane->count == count + 1);
    bool exact = lane->count == count + 1;
    for (unsigned i = 0; i < count && exact; ++i) exact = lane->captured[i].val == payload[i].val;
    check(phase, "rx_exact_payload", exact);
    bool guard = lane->count == count + 1 && lane->captured[count].level0 == payload[count].level0 &&
                 lane->captured[count].duration0 == payload[count].duration0 &&
                 lane->captured[count].level1 == payload[count].level1;
    check(phase, "rx_guard_pulse", guard);
}

static void send_payload(lane_t *lane, const char *phase, unsigned count, uint64_t ticks, uint32_t idle_ns)
{
    if (!arm(lane, 0, idle_ns, phase)) return;
    rmt_transmit_config_t cfg = {0};
    int64_t start = esp_timer_get_time();
    if (!status(phase, "transmit", rmt_transmit(lane->tx, encoder, payload, (count + 1) * sizeof(payload[0]), &cfg))) return;
    status(phase, "wait_tx", rmt_tx_wait_all_done(lane->tx, WAIT_MS));
    wait_receive(lane);
    dump(lane, phase, start);
    check(phase, "tx_single_done", lane->tx_events == 1 && lane->tx_symbols == count + 2);
    check(phase, "tx_not_immediate", lane->tx_done_us - start >= (int64_t)(ticks * 1000000 / lane->resolution));
    verify_capture(lane, phase, count);
    if (connected && !lane->dma && count > 48) check(phase, "rx_ring_partial_progress", lane->partial_events >= count / 48);
}

static void transfer(lane_t *lane, const char *phase, unsigned count)
{
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=%" PRIu32 " payload_symbols=%u\n", phase, lane->resolution, count);
    uint64_t ticks = vector(count, lane->resolution, phase);
    send_payload(lane, phase, count, ticks, IDLE_NS);
}

static uint64_t nec_payload(bool active_low)
{
    const uint8_t frame[] = {0x34, 0xcb, 0xa7, 0x58};
    uint64_t ticks = 13500;
    payload[0] = (rmt_symbol_word_t){.level0 = !active_low, .duration0 = 9000,
                                  .level1 = active_low, .duration1 = 4500};
    for (unsigned i = 0; i < 32; ++i) {
        unsigned bit = (frame[i / 8] >> (i % 8)) & 1;
        payload[i + 1] = (rmt_symbol_word_t){.level0 = !active_low, .duration0 = 560,
                                         .level1 = active_low, .duration1 = bit ? 1690 : 560};
        ticks += 560 + payload[i + 1].duration1;
    }
    payload[33] = (rmt_symbol_word_t){.level0 = !active_low, .duration0 = 560,
                                   .level1 = active_low, .duration1 = 1000};
    return ticks + 1560;
}

static void nec_case(lane_t *lane)
{
    const char *phase = "nec_ir";
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=1000000 payload_symbols=33\n", phase);
    printf("RMT_NATIVE_CONTENT phase=%s protocol=NEC address=34 command=a7 bytes=34cba758\n", phase);
    send_payload(lane, phase, 33, nec_payload(false), 12000000);
}

static void external_nec_case(void)
{
    const char *phase = "external_nec";
    lane_t *lane = &lanes[0];
    memset(lane, 0, sizeof(*lane));
    lane->resolution = 1000000;
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=1000000 payload_symbols=33\n", phase);
    printf("RMT_NATIVE_CONTENT phase=%s protocol=NEC address=34 command=a7 bytes=34cba758 origin=external_model\n", phase);
    gpio_config_t power_monitor = {
        .pin_bit_mask = UINT64_C(1) << 9, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (!status(phase, "power_monitor_input", gpio_config(&power_monitor))) return;
    check(phase, "initial_power_off", gpio_get_level(9) == 0);
    rmt_rx_channel_config_t cfg = {
        .gpio_num = 8, .clk_src = RMT_CLK_SRC_APB,
        .resolution_hz = 1000000, .mem_block_symbols = 48,
    };
    if (!status(phase, "new_rx", rmt_new_rx_channel(&cfg, &lane->rx))) return;
    rmt_rx_event_callbacks_t callbacks = {.on_recv_done = rx_done};
    if (!status(phase, "rx_callback", rmt_rx_register_event_callbacks(lane->rx, &callbacks, lane))) return;
    if (!status(phase, "enable_rx", rmt_enable(lane->rx))) return;
    nec_payload(true);
    if (!arm(lane, 1000, 12000000, phase)) return;
    int64_t armed = esp_timer_get_time();
    /* Host may only power the actual graph component, never supply RX data. */
    printf("RMT_NATIVE_EXTERNAL_READY rx_gpio=8 armed_us=%" PRId64 "\n", armed);
    fflush(stdout);
    /* GPIO9 is wired to the actual NEC VDD node. Host load may delay graph
     * Apply; deterministic icount must not turn that into a guest timeout.
     * The host watchdog bounds a missing physical power transition. */
    while (gpio_get_level(9) == 0) vTaskDelay(1);
    int64_t power_seen = esp_timer_get_time();
    printf("RMT_NATIVE_POWER phase=%s gpio=9 level=1 observed_us=%" PRId64 "\n", phase, power_seen);
    check(phase, "before_first_frame_empty", lane->rx_events == 0 && lane->count == 0 && !lane->last);
    while (!lane->last && esp_timer_get_time() - power_seen < 1000000) vTaskDelay(1);
    dump(lane, phase, armed);
    check(phase, "external_no_tx", lane->tx == NULL && lane->tx_events == 0);
    verify_capture(lane, phase, 33);
    if (connected) {
        uint8_t decoded[4] = {0};
        for (unsigned i = 0; i < 32 && lane->count == 34; ++i) {
            if (lane->captured[i + 1].duration1 == 1690) decoded[i / 8] |= 1U << (i % 8);
        }
        printf("RMT_NATIVE_DECODE phase=%s address=%02x command=%02x bytes=%02x%02x%02x%02x\n",
               phase, decoded[0], decoded[2], decoded[0], decoded[1], decoded[2], decoded[3]);
        check(phase, "external_nec_decode", lane->count == 34 && decoded[0] == 0x34 &&
              decoded[1] == 0xcb && decoded[2] == 0xa7 && decoded[3] == 0x58);
    }
    close_lane(lane, phase);
}

static void ws2812_case(lane_t *lane)
{
    const char *phase = "ws2812_grb";
    const uint8_t pixels[] = {0x12, 0x34, 0x56, 0x80, 0x01, 0xfe, 0xff, 0x00, 0x7f};
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=20000000 payload_symbols=72\n", phase);
    printf("RMT_NATIVE_CONTENT phase=%s protocol=WS2812 order=GRB pixels=3 bytes=1234568001feff007f\n", phase);
    for (unsigned i = 0; i < 72; ++i) {
        unsigned bit = (pixels[i / 8] >> (7 - i % 8)) & 1;
        payload[i] = (rmt_symbol_word_t){.level0 = 1, .duration0 = bit ? 16 : 8,
                                       .level1 = 0, .duration1 = bit ? 9 : 17};
    }
    if (!arm(lane, 0, IDLE_NS, phase)) return;
    rmt_transmit_config_t cfg = {0};
    int64_t start = esp_timer_get_time();
    status(phase, "transmit", rmt_transmit(lane->tx, encoder, payload, 72 * sizeof(payload[0]), &cfg));
    status(phase, "wait_tx", rmt_tx_wait_all_done(lane->tx, WAIT_MS));
    wait_receive(lane);
    dump(lane, phase, start);
    check(phase, "tx_single_done", lane->tx_events == 1 && lane->tx_symbols == 73);
    check(phase, "tx_not_immediate", lane->tx_done_us - start >= 90);
    if (connected) {
        check(phase, "rx_idle_completion", lane->last && !lane->overflow);
        check(phase, "rx_exact_count", lane->count == 72);
        bool exact = lane->count == 72;
        for (unsigned i = 0; i < 71 && exact; ++i) exact = lane->captured[i].val == payload[i].val;
        check(phase, "rx_exact_payload", exact);
        check(phase, "rx_final_bit", lane->count == 72 && lane->captured[71].level0 == 1 &&
              lane->captured[71].duration0 == payload[71].duration0 && lane->captured[71].level1 == 0);
        check(phase, "rx_ring_partial_progress", lane->partial_events >= 1);
    } else {
        check(phase, "disconnected_no_payload", lane->count == 0 && lane->rx_events == 0);
    }
}

static void filter_case(lane_t *lane)
{
    const char *phase = "noise_filter";
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=1000000 payload_symbols=1\n", phase);
    if (!arm(lane, 3000, IDLE_NS, phase)) return;
    rmt_symbol_word_t glitch = {.level0 = 1, .duration0 = 2, .level1 = 0, .duration1 = 1};
    rmt_transmit_config_t cfg = {0};
    int64_t start = esp_timer_get_time();
    status(phase, "transmit_glitch", rmt_transmit(lane->tx, encoder, &glitch, sizeof(glitch), &cfg));
    status(phase, "wait_glitch", rmt_tx_wait_all_done(lane->tx, WAIT_MS));
    vTaskDelay(pdMS_TO_TICKS(5));
    check(phase, "glitch_no_completion", lane->rx_events == 0 && lane->count == 0);
    rmt_symbol_word_t pulse = {.level0 = 1, .duration0 = 20, .level1 = 0, .duration1 = 10};
    status(phase, "transmit_valid", rmt_transmit(lane->tx, encoder, &pulse, sizeof(pulse), &cfg));
    status(phase, "wait_valid", rmt_tx_wait_all_done(lane->tx, WAIT_MS));
    wait_receive(lane);
    dump(lane, phase, start);
    if (connected) {
        check(phase, "valid_exact_pulse", lane->last && lane->count == 1 &&
              lane->captured[0].level0 == 1 && lane->captured[0].duration0 == 20 && lane->captured[0].level1 == 0);
    } else {
        check(phase, "disconnected_no_payload", lane->count == 0 && lane->rx_events == 0);
    }
}

static void disable_case(lane_t *lane)
{
    const char *phase = "disable_midstream";
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=1000000 payload_symbols=1\n", phase);
    if (!arm(lane, 1000, IDLE_NS, phase)) return;
    rmt_symbol_word_t cycle = {.level0 = 1, .duration0 = 200, .level1 = 0, .duration1 = 200};
    rmt_transmit_config_t cfg = {.loop_count = -1};
    int64_t start = esp_timer_get_time();
    status(phase, "transmit_loop", rmt_transmit(lane->tx, encoder, &cycle, sizeof(cycle), &cfg));
    esp_err_t waiting = rmt_tx_wait_all_done(lane->tx, 3);
    check(phase, "loop_wait_timeout", waiting == ESP_ERR_TIMEOUT);
    status(phase, "disable_working_tx", rmt_disable(lane->tx));
    int64_t stopped = esp_timer_get_time();
    wait_receive(lane);
    dump(lane, phase, start);
    check(phase, "no_fabricated_tx_done", lane->tx_events == 0);
    if (connected) {
        bool valid = lane->last && lane->count > 1 && lane->count < 20;
        for (size_t i = 0; i + 1 < lane->count && valid; ++i) valid = lane->captured[i].val == cycle.val;
        check(phase, "loop_prefix_exact", valid);
        check(phase, "stop_causes_idle", lane->last && lane->rx_done_us >= stopped);
    } else {
        check(phase, "disconnected_no_payload", lane->count == 0 && lane->rx_events == 0);
    }
    status(phase, "reenable_tx", rmt_enable(lane->tx));
    transfer(lane, "disable_recovery", 8);
}

static void carrier_case(lane_t *lane)
{
    const char *phase = "carrier";
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=1000000 payload_symbols=1\n", phase);
    rmt_carrier_config_t carrier = {.frequency_hz = 100000, .duty_cycle = 0.5f};
    if (!status(phase, "apply_carrier", rmt_apply_carrier(lane->tx, &carrier))) return;
    if (!arm(lane, 0, 300000, phase)) return;
    rmt_symbol_word_t envelope = {.level0 = 1, .duration0 = 100, .level1 = 0, .duration1 = 50};
    rmt_transmit_config_t cfg = {0};
    int64_t start = esp_timer_get_time();
    status(phase, "transmit", rmt_transmit(lane->tx, encoder, &envelope, sizeof(envelope), &cfg));
    status(phase, "wait_tx", rmt_tx_wait_all_done(lane->tx, WAIT_MS));
    wait_receive(lane);
    dump(lane, phase, start);
    if (connected) {
        bool exact = lane->last && lane->count == 10;
        for (size_t i = 0; i < 10 && exact; ++i) {
            rmt_symbol_word_t s = lane->captured[i];
            exact = s.level0 == 1 && s.duration0 == 5 && s.level1 == 0 && (i == 9 || s.duration1 == 5);
        }
        check(phase, "carrier_exact_raw_pulses", exact);
    } else {
        check(phase, "disconnected_no_payload", lane->count == 0 && lane->rx_events == 0);
    }
    status(phase, "remove_carrier", rmt_apply_carrier(lane->tx, NULL));
}

static void sync_case(void)
{
    const char *phase = "synchronization";
    printf("RMT_NATIVE_PHASE name=%s resolution_hz=1000000 payload_symbols=8\n", phase);
    if (!open_lane(&lanes[1], 1, RMT_CLK_SRC_APB, 1000000, false, phase)) return;
    rmt_channel_handle_t channels[] = {lanes[0].tx, lanes[1].tx};
    rmt_sync_manager_config_t cfg = {.tx_channel_array = channels, .array_size = 2};
    rmt_sync_manager_handle_t manager = NULL;
    if (!status(phase, "new_sync", rmt_new_sync_manager(&cfg, &manager))) return;
    vector(8, 1000000, phase);
    arm(&lanes[0], 1000, IDLE_NS, phase);
    arm(&lanes[1], 1000, IDLE_NS, phase);
    rmt_transmit_config_t transmit = {0};
    int64_t first = esp_timer_get_time();
    status(phase, "queue_first", rmt_transmit(lanes[0].tx, encoder, payload, 9 * sizeof(payload[0]), &transmit));
    vTaskDelay(pdMS_TO_TICKS(5));
    check(phase, "first_withheld", lanes[0].tx_events == 0 && lanes[0].rx_events == 0);
    /* Separate copy encoders prevent shared encoding state across channels. */
    rmt_encoder_handle_t second_encoder = NULL;
    rmt_copy_encoder_config_t copy = {};
    status(phase, "second_encoder", rmt_new_copy_encoder(&copy, &second_encoder));
    int64_t second = esp_timer_get_time();
    status(phase, "queue_second", rmt_transmit(lanes[1].tx, second_encoder, payload, 9 * sizeof(payload[0]), &transmit));
    for (unsigned i = 0; i < 2; ++i) {
        status(phase, "wait_tx", rmt_tx_wait_all_done(lanes[i].tx, WAIT_MS));
        wait_receive(&lanes[i]);
        dump(&lanes[i], phase, i ? second : first);
        check(phase, "tx_single_done", lanes[i].tx_events == 1 && lanes[i].tx_symbols == 10);
        verify_capture(&lanes[i], phase, 8);
    }
    check(phase, "both_start_after_second", lanes[0].tx_done_us > second && lanes[1].tx_done_us > second);
    status(phase, "reset_sync", rmt_sync_reset(manager));
    status(phase, "delete_sync", rmt_del_sync_manager(manager));
    status(phase, "delete_second_encoder", rmt_del_encoder(second_encoder));
    close_lane(&lanes[1], phase);
}

void app_main(void)
{
    printf("RMT_NATIVE_BOOT profile=%s idf=6.1 tx_gpio=4 rx_gpio=5 sync_tx_gpio=6 sync_rx_gpio=7\n", connected ? "connected" : "disconnected");
    printf("RMT_NATIVE_LIMIT name=idle_terminal_half qualification=printed_verbatim_not_specified_by_TRM\n");
    printf("RMT_NATIVE_LIMIT name=sync_start_timestamp qualification=requires_model_edge_trace_not_ISR_latency\n");
    external_nec_case();
    rmt_copy_encoder_config_t copy = {};
    if (status("setup", "copy_encoder", rmt_new_copy_encoder(&copy, &encoder))) {
        if (open_lane(&lanes[0], 0, RMT_CLK_SRC_APB, 1000000, false, "apb_1mhz")) {
            transfer(&lanes[0], "apb_1mhz", 8);
            transfer(&lanes[0], "long_refill", 120);
            filter_case(&lanes[0]);
            disable_case(&lanes[0]);
            carrier_case(&lanes[0]);
            sync_case();
            nec_case(&lanes[0]);
            close_lane(&lanes[0], "apb_1mhz");
        }
        if (open_lane(&lanes[0], 0, RMT_CLK_SRC_XTAL, 2000000, false, "xtal_2mhz")) {
            transfer(&lanes[0], "xtal_2mhz", 8);
            close_lane(&lanes[0], "xtal_2mhz");
        }
        if (open_lane(&lanes[0], 0, RMT_CLK_SRC_APB, 500000, false, "apb_500khz")) {
            transfer(&lanes[0], "apb_500khz", 8);
            close_lane(&lanes[0], "apb_500khz");
        }
        if (open_lane(&lanes[0], 0, RMT_CLK_SRC_APB, 20000000, false, "ws2812_grb")) {
            ws2812_case(&lanes[0]);
            close_lane(&lanes[0], "ws2812_grb");
        }
        if (open_lane(&lanes[0], 0, RMT_CLK_SRC_APB, 1000000, true, "dma_long")) {
            transfer(&lanes[0], "dma_long", 120);
            close_lane(&lanes[0], "dma_long");
        }
        status("cleanup", "delete_encoder", rmt_del_encoder(encoder));
    }
    printf("RMT_NATIVE_DONE profile=%s failures=%u\n", connected ? "connected" : "disconnected", failures);
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
