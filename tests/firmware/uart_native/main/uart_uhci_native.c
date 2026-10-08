#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/uhci.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* The ordinary APIs and RX callback lifetime pattern follow pinned IDF6.1
 * esp_driver_uart/test_apps/uhci/main/test_uhci.c, but physical peers replace
 * its self-connected GPIO. UART1 has no competing UART ISR driver. */
#define COUNT 513
#define TIMEOUT pdMS_TO_TICKS(3000)
typedef struct {
    bool rx, eof;
    size_t size;
} notice_t;
typedef struct {
    QueueHandle_t queue;
    size_t count;
    bool invalid;
    uint8_t received[1024];
} capture_t;
static unsigned failures;
static uint8_t to_peer[COUNT], from_peer[COUNT], peer_received[COUNT];

static void check(const char *name, bool ok)
{
    failures += !ok;
    printf("UHCI_NATIVE_CHECK name=%s result=%s\n", name, ok ? "PASS" : "FAIL");
}

static bool api(const char *name, esp_err_t result)
{
    printf("UHCI_NATIVE_API name=%s status=%d\n", name, (int)result);
    check(name, result == ESP_OK);
    return result == ESP_OK;
}

static bool received(uhci_controller_handle_t controller, const uhci_rx_event_data_t *data, void *opaque)
{
    (void)controller;
    capture_t *capture = opaque;
    /* Data is transient ISR callback storage. Retaining actual received
     * bytes requires this bounded copy; never substitute expected payload. */
    if (data->data == NULL || data->recv_size > sizeof(capture->received) - capture->count) {
        capture->invalid = true;
    } else {
        memcpy(capture->received + capture->count, data->data, data->recv_size);
        capture->count += data->recv_size;
    }
    notice_t notice = {.rx = true, .eof = data->flags.totally_received, .size = data->recv_size};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(capture->queue, &notice, &woken) != pdTRUE) {
        capture->invalid = true;
    }
    return woken == pdTRUE;
}

static bool transmitted(uhci_controller_handle_t controller, const uhci_tx_done_event_data_t *data, void *opaque)
{
    (void)controller;
    capture_t *capture = opaque;
    notice_t notice = {.rx = false, .size = data->sent_size};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(capture->queue, &notice, &woken) != pdTRUE) {
        capture->invalid = true;
    }
    return woken == pdTRUE;
}

static void bytes(const char *name, const uint8_t *data, size_t count)
{
    printf("UHCI_NATIVE_BYTES name=%s size=%u data=", name, (unsigned)count);
    for (size_t i = 0; i < count; ++i) { printf("%02x", data[i]); }
    printf("\n");
}

static int peer_read(void)
{
    int count = 0;
    int64_t deadline = esp_timer_get_time() + 3000000;
    while (count < COUNT && esp_timer_get_time() < deadline) {
        int got = uart_read_bytes(UART_NUM_2, peer_received + count, COUNT - count, pdMS_TO_TICKS(25));
        if (got < 0) { return got; }
        count += got;
    }
    return count;
}

static bool round_trip(const char *name, unsigned salt, unsigned eof_mode, bool multi,
                       uint8_t *dma_buffer, capture_t *capture, void (*console_probe)(void))
{
    memset(capture->received, 0, sizeof(capture->received));
    capture->count = 0;
    capture->invalid = false;
    xQueueReset(capture->queue);
    for (unsigned i = 0; i < COUNT; ++i) {
        to_peer[i] = (i * 29 + salt + 11) & 255;
        from_peer[i] = (i * 47 + salt + 101) & 255;
    }
    /* Break EOF tests exclude zero data values: NULL frames are meaningful
     * break terminators in this configured UHCI mode, not arbitrary payload. */
    if (eof_mode == 2) {
        for (unsigned i = 0; i < COUNT; ++i) {
            if (!from_peer[i]) { from_peer[i] = 0x80; }
        }
    }
    uart_config_t uart = {.baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_XTAL};
    api("selected_uart_config", uart_param_config(UART_NUM_1, &uart));
    api("selected_uart_pins", uart_set_pin(UART_NUM_1, 17, 15, -1, -1));
    api("peer_flush", uart_flush_input(UART_NUM_2));
    uhci_controller_config_t cfg = {.uart_port = UART_NUM_1,
        .tx_trans_queue_depth = 4, .max_transmit_size = 2048,
        .max_transmit_buffer_count = 3, .max_receive_internal_mem = 4096,
        .dma_burst_size = 32, .max_packet_receive = COUNT};
    cfg.rx_eof_flags.idle_eof = eof_mode == 0;
    cfg.rx_eof_flags.length_eof = eof_mode == 1;
    cfg.rx_eof_flags.rx_brk_eof = eof_mode == 2;
    uhci_controller_handle_t controller = NULL;
    if (!api("new_controller", uhci_new_controller(&cfg, &controller))) { return true; }
    uhci_event_callbacks_t callbacks = {.on_rx_trans_event = received, .on_tx_trans_done = transmitted};
    bool ready = api("callbacks", uhci_register_event_callbacks(controller, &callbacks, capture));
    ready = ready && api("receive_arm", uhci_receive(controller, dma_buffer, 256));
    int64_t started = esp_timer_get_time();
    if (ready) {
        if (multi) {
            uhci_transmit_buffer_info_t segments[] = {
                {.write_buffer = to_peer, .buffer_size = 129},
                {.write_buffer = to_peer + 129, .buffer_size = 257},
                {.write_buffer = to_peer + 386, .buffer_size = 127},
            };
            api("multi_buffer_transmit", uhci_multi_buffer_transmit(controller, segments, 3));
        } else {
            api("transmit", uhci_transmit(controller, to_peer, COUNT));
        }
        int written = eof_mode == 2 ?
            uart_write_bytes_with_break(UART_NUM_2, from_peer, COUNT, 30) :
            uart_write_bytes(UART_NUM_2, from_peer, COUNT);
        check("peer_write513", written == COUNT);
        /* While UART1 DMA TX/RX and ordinary UART2 are live, exercise the
         * independent physical UART0 net. That function logs only after
         * restoring its console routes, never onto its test loopback. */
        if (console_probe != NULL) {
            int64_t probe_begin = esp_timer_get_time() - started;
            console_probe();
            printf("UHCI_NATIVE_CONCURRENT uart0_probe_begin_us=%lld uart0_probe_end_us=%lld\n",
                   (long long)probe_begin, (long long)(esp_timer_get_time() - started));
            check("concurrent_uart0_before_physical_tx_end", probe_begin < 512LL * 10 * 1000000 / 115200);
        }
        bool rx_eof = false, tx_done = false;
        unsigned partial = 0;
        size_t tx_size = 0;
        int64_t deadline = esp_timer_get_time() + 3000000;
        while ((!rx_eof || !tx_done) && esp_timer_get_time() < deadline) {
            notice_t notice;
            if (xQueueReceive(capture->queue, &notice, pdMS_TO_TICKS(25)) != pdTRUE) { continue; }
            printf("UHCI_NATIVE_EVENT name=%s rx=%d eof=%d size=%u\n", name,
                   notice.rx, notice.eof, (unsigned)notice.size);
            if (notice.rx) {
                rx_eof |= notice.eof;
                partial += !notice.eof;
            } else {
                tx_done = true;
                tx_size = notice.size;
            }
        }
        esp_err_t dma_done = uhci_wait_all_tx_transaction_done(controller, 3000);
        esp_err_t uart_done = uart_wait_tx_done(UART_NUM_2, TIMEOUT);
        int peer_count = peer_read();
        bool exact = rx_eof && tx_done && tx_size == COUNT && !capture->invalid &&
            capture->count == COUNT && peer_count == COUNT &&
            !memcmp(capture->received, from_peer, COUNT) && !memcmp(peer_received, to_peer, COUNT) &&
            dma_done == ESP_OK && uart_done == ESP_OK;
        printf("UHCI_NATIVE_TRANSFER name=%s salt=%u eof_mode=%u multi=%d rx=%u peer_rx=%d tx_callback=%u partial=%u elapsed_us=%lld\n",
               name, salt, eof_mode, multi, (unsigned)capture->count, peer_count,
               (unsigned)tx_size, partial, (long long)(esp_timer_get_time() - started));
        check(name, exact);
        check("real_partial_dma_callbacks", partial > 0);
        if (capture->count <= sizeof(capture->received)) { bytes("dma_rx", capture->received, capture->count); }
        if (peer_count > 0) { bytes("peer_rx", peer_received, peer_count); }
    }
    return api("controller_delete_reset", uhci_del_controller(controller));
}

unsigned uart_uhci_native_run(void (*console_probe)(void))
{
    failures = 0;
    printf("UHCI_NATIVE_BOOT controller=0 selected_uart=1 tx=17 rx=15 peer_uart=2 peer_tx=18 peer_rx=16\n");
    uart_config_t peer = {.baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_XTAL};
    if (!api("peer_driver_install", uart_driver_install(UART_NUM_2, 4096, 4096, 0, NULL, 0))) { return failures; }
    bool ready = api("peer_config", uart_param_config(UART_NUM_2, &peer));
    ready = api("peer_pins", uart_set_pin(UART_NUM_2, 18, 16, -1, -1)) && ready;
    /* Keep callback storage alive even if an ordinary API cannot delete an
     * unfinished RX after a failed model run. Never free an active DMA owner. */
    static capture_t capture;
    capture.queue = xQueueCreate(32, sizeof(notice_t));
    uint8_t *dma_buffer = heap_caps_malloc(256, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    check("capture_queue", capture.queue != NULL);
    check("dma_buffer", dma_buffer != NULL);
    check("selected_uart_no_competing_driver", !uart_is_driver_installed(UART_NUM_1));
    bool released = true;
    if (ready && capture.queue != NULL && dma_buffer != NULL) {
        released = round_trip("idle_full_duplex513_concurrent_uart0", 5, 0, false, dma_buffer, &capture, console_probe);
        if (released) { released = round_trip("idle_multibuffer_reset513", 13, 0, true, dma_buffer, &capture, NULL); }
        if (released) { released = round_trip("length_eof_reset513", 23, 1, false, dma_buffer, &capture, NULL); }
        if (released) { released = round_trip("break_eof_reset513", 37, 2, false, dma_buffer, &capture, NULL); }
    }
    if (released && dma_buffer != NULL) { heap_caps_free(dma_buffer); }
    if (released && capture.queue != NULL) { vQueueDelete(capture.queue); }
    api("peer_driver_delete", uart_driver_delete(UART_NUM_2));
    printf("UHCI_NATIVE_UNAVAILABLE ordinary_api=packet_separator_configuration,escape_sequence_configuration public_driver=driver/uhci.h HAL=init_deinit_only\n");
    printf("UHCI_NATIVE_DONE failures=%u result=%s\n", failures, failures ? "FAIL" : "PASS");
    return failures;
}
