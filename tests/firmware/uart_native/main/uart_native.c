#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "soc/uart_reg.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

unsigned uart_uhci_native_run(void (*console_probe)(void));

/* No internal loopback, register access, console RX injection, or echo peer.
 * The two TX payloads are independent; every RX assertion compares the other
 * controller's actual payload. All GPIO connectivity belongs to the v3 graph. */
#define TX1 17
#define RX1 15
#define TX2 18
#define RX2 16
#define RTS1 11
#define CTS1 12
#define RTS2 13
#define CTS2 14
#define TRANSFER 513
#define WAIT pdMS_TO_TICKS(2500)
static QueueHandle_t events[3];
static unsigned failures;
static uint8_t tx1[TRANSFER], tx2[TRANSFER], rx1[TRANSFER], rx2[TRANSFER];
#if CONFIG_UART_NATIVE_PROFILE_ABSENT
static const char *profile = "absent";
#elif CONFIG_UART_NATIVE_PROFILE_DISCONNECTED
static const char *profile = "disconnected";
#elif CONFIG_UART_NATIVE_PROFILE_WRONG
static const char *profile = "wrong";
#elif CONFIG_UART_NATIVE_PROFILE_UHCI
static const char *profile = "uhci";
#else
static const char *profile = "connected";
#endif

static void check(const char *name, bool ok)
{
    failures += !ok;
    printf("UART_NATIVE_CHECK name=%s result=%s\n", name, ok ? "PASS" : "FAIL");
}

static bool api(const char *name, esp_err_t result)
{
    printf("UART_NATIVE_API name=%s status=%d\n", name, (int)result);
    check(name, result == ESP_OK);
    return result == ESP_OK;
}

static uart_config_t config(unsigned baud, uart_word_length_t bits,
                            uart_parity_t parity, uart_stop_bits_t stops)
{
    return (uart_config_t) {
        .baud_rate = baud, .data_bits = bits, .parity = parity,
        .stop_bits = stops, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 96, .source_clk = UART_SCLK_XTAL,
    };
}

static bool setup(uart_port_t port, int tx, int rx, int rts, int cts, int rxsize)
{
    uart_config_t cfg = config(115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1);
    if (!api("driver_install", uart_driver_install(port, rxsize, 4096, 64, &events[port], 0))) {
        return false;
    }
    return api("param_config", uart_param_config(port, &cfg)) &&
           api("set_pin", uart_set_pin(port, tx, rx, rts, cts)) &&
           api("rx_threshold", uart_set_rx_full_threshold(port, 32)) &&
           api("rx_timeout", uart_set_rx_timeout(port, 4));
}

static void clean(uart_port_t port)
{
    api("flush_input", uart_flush_input(port));
    xQueueReset(events[port]);
}

static void payloads(unsigned salt, unsigned mask)
{
    for (unsigned i = 0; i < TRANSFER; ++i) {
        tx1[i] = (i * 37 + salt + 3) & mask;
        tx2[i] = (i * 53 + salt + 91) & mask;
    }
}

static int read_exact(uart_port_t port, uint8_t *data, int count)
{
    int total = 0;
    int64_t deadline = esp_timer_get_time() + 2500000;
    while (total < count && esp_timer_get_time() < deadline) {
        int n = uart_read_bytes(port, data + total, count - total, pdMS_TO_TICKS(25));
        if (n < 0) { return n; }
        total += n;
    }
    return total;
}

static void bytes(const char *name, const uint8_t *data, unsigned count)
{
    printf("UART_NATIVE_BYTES name=%s size=%u data=", name, count);
    for (unsigned i = 0; i < count; ++i) { printf("%02x", data[i]); }
    printf("\n");
}

static bool event(uart_port_t port, uart_event_type_t type, int timeout_kind)
{
    uart_event_t ev;
    bool seen = false;
    while (xQueueReceive(events[port], &ev, pdMS_TO_TICKS(25)) == pdTRUE) {
        printf("UART_NATIVE_EVENT port=%d type=%d size=%u timeout=%d\n",
               port, ev.type, (unsigned)ev.size, ev.timeout_flag);
        if (ev.type == type && (timeout_kind < 0 || ev.timeout_flag == timeout_kind)) {
            seen = true;
        }
    }
    return seen;
}

static void transfer(const char *name, unsigned baud, uart_word_length_t bits,
                     uart_parity_t parity, uart_stop_bits_t stops, unsigned salt)
{
    uart_config_t cfg = config(baud, bits, parity, stops);
    api("config1", uart_param_config(UART_NUM_1, &cfg));
    api("config2", uart_param_config(UART_NUM_2, &cfg));
    clean(UART_NUM_1);
    clean(UART_NUM_2);
    unsigned mask = (1U << ((unsigned)bits + 5)) - 1;
    payloads(salt, mask);
    int64_t start = esp_timer_get_time();
    check("write1_full", uart_write_bytes(UART_NUM_1, tx1, TRANSFER) == TRANSFER);
    check("write2_full", uart_write_bytes(UART_NUM_2, tx2, TRANSFER) == TRANSFER);
    int n1 = read_exact(UART_NUM_1, rx1, TRANSFER);
    int n2 = read_exact(UART_NUM_2, rx2, TRANSFER);
    esp_err_t done1 = uart_wait_tx_done(UART_NUM_1, WAIT);
    esp_err_t done2 = uart_wait_tx_done(UART_NUM_2, WAIT);
    bool ok = n1 == TRANSFER && n2 == TRANSFER &&
              !memcmp(tx2, rx1, TRANSFER) && !memcmp(tx1, rx2, TRANSFER) &&
              done1 == ESP_OK && done2 == ESP_OK;
    printf("UART_NATIVE_TRANSFER name=%s baud=%u bits=%u parity=%d stops=%d n1=%d n2=%d elapsed_us=%lld\n",
           name, baud, (unsigned)bits + 5, parity, stops, n1, n2,
           (long long)(esp_timer_get_time() - start));
    check(name, ok);
    if (n1 > 0) { bytes("rx1", rx1, n1); }
    if (n2 > 0) { bytes("rx2", rx2, n2); }
}

static void threshold_timeout(void)
{
    clean(UART_NUM_2);
    api("threshold64", uart_set_rx_full_threshold(UART_NUM_2, 64));
    api("timeout_disabled", uart_set_rx_timeout(UART_NUM_2, 0));
    payloads(29, 255);
    check("threshold_write", uart_write_bytes(UART_NUM_1, tx1, 65) == 65);
    api("threshold_tx_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("threshold_irq", event(UART_NUM_2, UART_DATA, 0));
    api("timeout_enabled", uart_set_rx_timeout(UART_NUM_2, 4));
    check("threshold_tail_exact", read_exact(UART_NUM_2, rx2, 65) == 65 && !memcmp(tx1, rx2, 65));
    clean(UART_NUM_2);
    check("timeout_write", uart_write_bytes(UART_NUM_1, tx1, 7) == 7);
    api("timeout_tx_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("timeout_irq", event(UART_NUM_2, UART_DATA, 1));
    check("timeout_exact", read_exact(UART_NUM_2, rx2, 7) == 7 && !memcmp(tx1, rx2, 7));
    api("threshold_restore", uart_set_rx_full_threshold(UART_NUM_2, 32));
}

static void routing_reset(void)
{
    clean(UART_NUM_1);
    clean(UART_NUM_2);
    api("disconnect_rx2", uart_set_pin(UART_NUM_2, TX2, 21, RTS2, CTS2));
    check("disconnect_write", uart_write_bytes(UART_NUM_1, tx1, 31) == 31);
    api("disconnect_tx_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("disconnected_no_receive", uart_read_bytes(UART_NUM_2, rx2, 31, pdMS_TO_TICKS(50)) == 0);
    api("reconnect_rx2", uart_set_pin(UART_NUM_2, TX2, RX2, RTS2, CTS2));
    transfer("routing_reconnect", 115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, 41);
    api("delete2_reset", uart_driver_delete(UART_NUM_2));
    if (!setup(UART_NUM_2, TX2, RX2, RTS2, CTS2, 4096)) { return; }
    transfer("driver_reset_reconnect", 115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, 47);
}

static void cts_stall(void)
{
    clean(UART_NUM_2);
    api("cts_enable", uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_CTS, 0));
    /* GPIO13 is the *physical* source on the CTS1 net, not a UART register
     * shortcut. Reset removes the UART RTS2 route before driving this pad. */
    api("rts2_gpio_reset", gpio_reset_pin(RTS2));
    api("rts2_gpio_output", gpio_set_direction(RTS2, GPIO_MODE_OUTPUT));
    api("cts_block_high", gpio_set_level(RTS2, 1));
    vTaskDelay(pdMS_TO_TICKS(2));
    payloads(67, 255);
    check("cts_enqueue513", uart_write_bytes(UART_NUM_1, tx1, TRANSFER) == TRANSFER);
    check("cts_tx_stalled", uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(30)) == ESP_ERR_TIMEOUT);
    check("cts_rx_stalled", uart_read_bytes(UART_NUM_2, rx2, TRANSFER, pdMS_TO_TICKS(20)) == 0);
    printf("UART_NATIVE_HOST_STOP_READY queued=513 cts=blocked control_gpio=10\n");
    int64_t host_deadline = esp_timer_get_time() + 10000000;
    while (!gpio_get_level(10) && esp_timer_get_time() < host_deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    check("host_stop_resume_physical_gate", gpio_get_level(10) == 1);
    api("cts_resume_low", gpio_set_level(RTS2, 0));
    check("cts_resume_exact", read_exact(UART_NUM_2, rx2, TRANSFER) == TRANSFER && !memcmp(tx1, rx2, TRANSFER));
    api("cts_resumed_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    api("cts_disable", uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_DISABLE, 0));
    api("rts2_restore", uart_set_pin(UART_NUM_2, TX2, RX2, RTS2, CTS2));
    clean(UART_NUM_2);
    api("auto_rts_enable", uart_set_hw_flow_ctrl(UART_NUM_2, UART_HW_FLOWCTRL_RTS, 32));
    api("auto_cts_enable", uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_CTS, 0));
    api("auto_rts_hold_rx", uart_disable_rx_intr(UART_NUM_2));
    check("auto_rts_enqueue513", uart_write_bytes(UART_NUM_1, tx1, TRANSFER) == TRANSFER);
    check("auto_rts_tx_stalled", uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(30)) == ESP_ERR_TIMEOUT);
    api("auto_rts_service_rx", uart_enable_rx_intr(UART_NUM_2));
    check("auto_rts_resume_exact", read_exact(UART_NUM_2, rx2, TRANSFER) == TRANSFER && !memcmp(tx1, rx2, TRANSFER));
    api("auto_rts_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    api("auto_cts_disable", uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_DISABLE, 0));
    api("auto_rts_disable", uart_set_hw_flow_ctrl(UART_NUM_2, UART_HW_FLOWCTRL_DISABLE, 0));
}

static void malformed(void)
{
    clean(UART_NUM_2);
    api("wrong_parity_tx", uart_set_parity(UART_NUM_1, UART_PARITY_EVEN));
    api("wrong_parity_rx", uart_set_parity(UART_NUM_2, UART_PARITY_ODD));
    check("wrong_parity_write", uart_write_bytes(UART_NUM_1, tx1, 17) == 17);
    api("wrong_parity_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("parity_error_irq", event(UART_NUM_2, UART_PARITY_ERR, -1));
    api("parity_restore1", uart_set_parity(UART_NUM_1, UART_PARITY_DISABLE));
    api("parity_restore2", uart_set_parity(UART_NUM_2, UART_PARITY_DISABLE));
    clean(UART_NUM_2);
    /* The pinned driver's default mask excludes framing errors. Request
     * that event explicitly through its public interrupt-control API. */
    api("frame_error_irq_enable",
        uart_enable_intr_mask(UART_NUM_2, UART_FRM_ERR_INT_ENA_M));
    api("wrong_width_tx", uart_set_word_length(UART_NUM_1, UART_DATA_8_BITS));
    api("wrong_width_rx", uart_set_word_length(UART_NUM_2, UART_DATA_5_BITS));
    uint8_t bad[64];
    memset(bad, 0, sizeof(bad)); /* RX expects a stop bit while TX still sends zero data bits. */
    check("wrong_width_write", uart_write_bytes(UART_NUM_1, bad, sizeof(bad)) == sizeof(bad));
    api("wrong_width_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("width_frame_error_irq", event(UART_NUM_2, UART_FRAME_ERR, -1));
    api("width_restore", uart_set_word_length(UART_NUM_2, UART_DATA_8_BITS));
    clean(UART_NUM_2);
    api("wrong_stop_tx", uart_set_stop_bits(UART_NUM_1, UART_STOP_BITS_1));
    api("wrong_stop_rx", uart_set_stop_bits(UART_NUM_2, UART_STOP_BITS_2));
    check("wrong_stops_write", uart_write_bytes(UART_NUM_1, bad, sizeof(bad)) == sizeof(bad));
    api("wrong_stops_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    /* Some UARTs validate only the first stop sample. Evidence must show an
     * error or changed stream; do not invent an error for an accepted frame. */
    bool stop_error = event(UART_NUM_2, UART_FRAME_ERR, -1);
    int n = uart_read_bytes(UART_NUM_2, rx2, sizeof(bad), pdMS_TO_TICKS(100));
    printf("UART_NATIVE_WRONG_STOPS frame_error=%d received=%d\n", stop_error, n);
    check("stop_mismatch_observed", stop_error || n != sizeof(bad) || memcmp(bad, rx2, sizeof(bad)));
    api("stop_restore", uart_set_stop_bits(UART_NUM_2, UART_STOP_BITS_1));
    clean(UART_NUM_2);
    check("break_write", uart_write_bytes_with_break(UART_NUM_1, tx1, 1, 30) == 1);
    api("break_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("break_irq", event(UART_NUM_2, UART_BREAK, -1));
    clean(UART_NUM_2);
    /* Disable ordinary RX service explicitly so real FIFO overload is
     * possible; re-enable service to observe the sticky overflow interrupt. */
    api("overload_disable_rx", uart_disable_rx_intr(UART_NUM_2));
    check("overload_write513", uart_write_bytes(UART_NUM_1, tx1, TRANSFER) == TRANSFER);
    api("overload_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    api("overload_enable_rx", uart_enable_rx_intr(UART_NUM_2));
    check("fifo_overflow_irq", event(UART_NUM_2, UART_FIFO_OVF, -1));
    clean(UART_NUM_2);
    api("ring_overload_delete", uart_driver_delete(UART_NUM_2));
    if (!setup(UART_NUM_2, TX2, RX2, RTS2, CTS2, 256)) { return; }
    for (unsigned i = 0; i < 4; ++i) {
        check("ring_overload_write", uart_write_bytes(UART_NUM_1, tx1, TRANSFER) == TRANSFER);
    }
    api("ring_overload_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("ring_buffer_full_irq", event(UART_NUM_2, UART_BUFFER_FULL, -1));
    api("ring_overload_reset", uart_driver_delete(UART_NUM_2));
    if (!setup(UART_NUM_2, TX2, RX2, RTS2, CTS2, 4096)) { return; }
    transfer("after_error_recovery", 115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, 73);
}

static void at_pattern(void)
{
    clean(UART_NUM_2);
    api("at_queue", uart_pattern_queue_reset(UART_NUM_2, 8));
    api("at_enable", uart_enable_pattern_det_baud_intr(UART_NUM_2, '+', 3, 20, 0, 0));
    const char pattern[] = "xy+++z";
    check("at_write", uart_write_bytes(UART_NUM_1, pattern, sizeof(pattern) - 1) == sizeof(pattern) - 1);
    api("at_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("at_irq", event(UART_NUM_2, UART_PATTERN_DET, -1));
    check("at_position", uart_pattern_pop_pos(UART_NUM_2) == 2);
    check("at_bytes", read_exact(UART_NUM_2, rx2, sizeof(pattern) - 1) == sizeof(pattern) - 1 && !memcmp(pattern, rx2, sizeof(pattern) - 1));
    api("at_guard_enable", uart_enable_pattern_det_baud_intr(UART_NUM_2, '+', 3, 20, 100, 100));
    clean(UART_NUM_2);
    check("at_guard_reject_write", uart_write_bytes(UART_NUM_1, pattern, sizeof(pattern) - 1) == sizeof(pattern) - 1);
    api("at_guard_reject_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("at_guard_reject", !event(UART_NUM_2, UART_PATTERN_DET, -1));
    clean(UART_NUM_2);
    vTaskDelay(pdMS_TO_TICKS(5));
    check("at_guard_accept_write", uart_write_bytes(UART_NUM_1, "+++", 3) == 3);
    api("at_guard_accept_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    vTaskDelay(pdMS_TO_TICKS(5));
    check("at_guard_accept", event(UART_NUM_2, UART_PATTERN_DET, -1));
    api("at_gap_enable", uart_enable_pattern_det_baud_intr(UART_NUM_2, '+', 3, 20, 0, 0));
    clean(UART_NUM_2);
    for (unsigned i = 0; i < 3; ++i) {
        check("at_gap_write", uart_write_bytes(UART_NUM_1, "+", 1) == 1);
        api("at_gap_done", uart_wait_tx_done(UART_NUM_1, WAIT));
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    check("at_gap_reject", !event(UART_NUM_2, UART_PATTERN_DET, -1));
    api("at_disable", uart_disable_pattern_det_intr(UART_NUM_2));
    clean(UART_NUM_2);
}

static void autobaud(void)
{
    clean(UART_NUM_2);
    /* Already acquired: IDF retains the configured XTAL source and RX pad. */
    api("autobaud_start", uart_detect_bitrate_start(UART_NUM_2, NULL));
    uint8_t alternating[32];
    memset(alternating, 0x55, sizeof(alternating));
    check("autobaud_write", uart_write_bytes(UART_NUM_1, alternating, sizeof(alternating)) == sizeof(alternating));
    api("autobaud_tx_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    uart_bitrate_res_t res = {0};
    api("autobaud_stop", uart_detect_bitrate_stop(UART_NUM_2, false, &res));
    uint64_t sum = (uint64_t)res.low_period + res.high_period;
    uint64_t measured = sum ? (uint64_t)res.clk_freq_hz * 2 / sum : 0;
    printf("UART_NATIVE_AUTOBAUD low=%lu high=%lu positive=%lu negative=%lu edges=%lu clock=%lu measured=%llu\n",
           (unsigned long)res.low_period, (unsigned long)res.high_period,
           (unsigned long)res.pos_period, (unsigned long)res.neg_period,
           (unsigned long)res.edge_cnt, (unsigned long)res.clk_freq_hz, (unsigned long long)measured);
    check("autobaud_actual_edges", res.edge_cnt > 0 && measured >= 112896 && measured <= 117504);
    clean(UART_NUM_2);
}

static void irda_receive_only(void)
{
    /* S3 IrDA is half duplex. uart_set_mode only sets IRDA_EN; the pinned
     * ordinary API has no IRDA_TX_EN/WCTL setter. Default direction receives
     * idle-HIGH, active-LOW pulses. Supply those through real GPIO17->RX16,
     * never enable unavailable UART TX using a LL/register helper. */
    static const uint8_t expected[] = {0x00, 0x55, 0xaa, 0xff};
    api("irda_gpio_source_reset", gpio_reset_pin(TX1));
    api("irda_gpio_source_idle", gpio_set_level(TX1, 1));
    api("irda_gpio_source_output", gpio_set_direction(TX1, GPIO_MODE_OUTPUT));
    uart_config_t cfg = config(9600, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1);
    api("irda_receiver_config", uart_param_config(UART_NUM_2, &cfg));
    api("irda_receiver_inverse_default", uart_set_line_inverse(UART_NUM_2, UART_SIGNAL_INV_DISABLE));
    api("irda_receive_mode", uart_set_mode(UART_NUM_2, UART_MODE_IRDA));
    clean(UART_NUM_2);
    bool driven = true;
    portMUX_TYPE pulse_lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&pulse_lock);
    for (unsigned byte = 0; byte < sizeof(expected); ++byte) {
        uint16_t frame = ((uint16_t)expected[byte] << 1) | (1U << 9);
        for (unsigned bit = 0; bit < 10; ++bit) {
            if (!(frame & (1U << bit))) {
                /* 9600 baud: one bit ~104us; zero pulse occupies 9th/10th/
                 * 11th sixteenth-cycles (ticks8..10), not the first3. */
                esp_rom_delay_us(52);
                driven &= gpio_set_level(TX1, 0) == ESP_OK;
                esp_rom_delay_us(20);
                driven &= gpio_set_level(TX1, 1) == ESP_OK;
                esp_rom_delay_us(32);
            } else {
                esp_rom_delay_us(104);
            }
        }
        esp_rom_delay_us(208);
    }
    portEXIT_CRITICAL(&pulse_lock);
    check("irda_gpio_physical_pulses", driven);
    int count = read_exact(UART_NUM_2, rx2, sizeof(expected));
    check("irda_receive_physical", count == sizeof(expected) && !memcmp(expected, rx2, sizeof(expected)));
    if (count > 0) { bytes("irda_rx", rx2, count); }
    api("irda_normal_restore", uart_set_mode(UART_NUM_2, UART_MODE_UART));
    cfg = config(115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1);
    api("irda_receiver_baud_restore", uart_param_config(UART_NUM_2, &cfg));
    api("irda_uart1_tx_route_restore", uart_set_pin(UART_NUM_1, TX1, RX1, RTS1, CTS1));
    clean(UART_NUM_2);
}

static void advanced_modes(void)
{
    irda_receive_only();
    payloads(79, 255);
    api("rs485_half1", uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX));
    clean(UART_NUM_2);
    check("rs485_half_write", uart_write_bytes(UART_NUM_1, tx1, TRANSFER) == TRANSFER);
    check("rs485_half_receive", read_exact(UART_NUM_2, rx2, TRANSFER) == TRANSFER && !memcmp(tx1, rx2, TRANSFER));
    api("rs485_half_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    api("rs485_normal", uart_set_mode(UART_NUM_1, UART_MODE_UART));
    api("rs485_app_mode", uart_set_mode(UART_NUM_1, UART_MODE_RS485_APP_CTRL));
    clean(UART_NUM_2);
    check("rs485_app_write", uart_write_bytes(UART_NUM_1, tx1, 129) == 129);
    check("rs485_app_receive", read_exact(UART_NUM_2, rx2, 129) == 129 && !memcmp(tx1, rx2, 129));
    api("rs485_app_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    api("rs485_self_receive_route", uart_set_pin(UART_NUM_1, TX1, RX2, RTS1, CTS1));
    api("rs485_no_collision_mode", uart_set_mode(UART_NUM_1, UART_MODE_RS485_COLLISION_DETECT));
    check("rs485_no_collision_write", uart_write_bytes(UART_NUM_1, tx1, 129) == 129);
    api("rs485_no_collision_done", uart_wait_tx_done(UART_NUM_1, WAIT));
    bool no_collision = true;
    api("rs485_no_collision_flag", uart_get_collision_flag(UART_NUM_1, &no_collision));
    check("rs485_no_collision", !no_collision);
    api("rs485_peer_receive_route", uart_set_pin(UART_NUM_1, TX1, RX1, RTS1, CTS1));
    api("rs485_collision1", uart_set_mode(UART_NUM_1, UART_MODE_RS485_COLLISION_DETECT));
    /* RX1 sees TX2, deliberately different while TX1 is busy. This exercises
     * the UART's physical comparison, not an invented differential bus. */
    clean(UART_NUM_1);
    check("rs485_collision_write1", uart_write_bytes(UART_NUM_1, tx1, TRANSFER) == TRANSFER);
    check("rs485_collision_write2", uart_write_bytes(UART_NUM_2, tx2, TRANSFER) == TRANSFER);
    api("rs485_collision_done1", uart_wait_tx_done(UART_NUM_1, WAIT));
    api("rs485_collision_done2", uart_wait_tx_done(UART_NUM_2, WAIT));
    bool collision = false;
    api("rs485_collision_flag", uart_get_collision_flag(UART_NUM_1, &collision));
    check("rs485_collision_observed", collision);
    api("rs485_restore", uart_set_mode(UART_NUM_1, UART_MODE_UART));
    clean(UART_NUM_1);
    clean(UART_NUM_2);
    printf("UART_NATIVE_SCOPE rs485=digital_UART_mode_logic differential_transceiver=not_present irda=receive_only_electrical_pulses irda_tx_direction=ordinary_api_unavailable optical_medium=not_present\n");
}

static void uart0_physical(void)
{
    /* stdout flushes into the hardware FIFO, not onto the wire. Drain before
     * installing/reconfiguring UART0: uart_param_config resets that FIFO.
     * Keep reports out of the GPIO4/5 transfer and print after restoring the
     * GPIO43/44 console route. The chardev still observes the binary TX bytes. */
    fflush(stdout);
    esp_err_t console_drain = uart_wait_tx_idle_polling(UART_NUM_0);
    uart_config_t cfg = config(57600, UART_DATA_8_BITS, UART_PARITY_EVEN, UART_STOP_BITS_2);
    esp_err_t install = uart_driver_install(UART_NUM_0, 4096, 4096, 32, &events[0], 0);
    /* Installation can log its queue state; drain that output before the
     * parameter update too, without printing another message here. */
    fflush(stdout);
    esp_err_t install_drain = uart_wait_tx_idle_polling(UART_NUM_0);
    esp_err_t configure = uart_param_config(UART_NUM_0, &cfg);
    esp_err_t pins = uart_set_pin(UART_NUM_0, 4, 5, -1, -1);
    esp_err_t timeout = uart_set_rx_timeout(UART_NUM_0, 4);
    payloads(83, 255);
    int sent = uart_write_bytes(UART_NUM_0, tx1, TRANSFER);
    int received = read_exact(UART_NUM_0, rx1, TRANSFER);
    esp_err_t done = uart_wait_tx_done(UART_NUM_0, WAIT);
    bool exact = received == TRANSFER && !memcmp(tx1, rx1, TRANSFER);
    uart_config_t console = config(115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1);
    esp_err_t restore_config = uart_param_config(UART_NUM_0, &console);
    esp_err_t restore_pins = uart_set_pin(UART_NUM_0, 43, 44, -1, -1);
    esp_err_t deleted = uart_driver_delete(UART_NUM_0);
    printf("\n");
    check("uart0_install", console_drain == ESP_OK && install == ESP_OK);
    check("uart0_config", install_drain == ESP_OK && configure == ESP_OK &&
          timeout == ESP_OK && pins == ESP_OK);
    check("uart0_physical_loopback513", sent == TRANSFER && exact && done == ESP_OK);
    check("uart0_console_restore", restore_config == ESP_OK && restore_pins == ESP_OK && deleted == ESP_OK);
    printf("UART_NATIVE_SCOPE uart0_external_tx=4 uart0_external_rx=5 console_tx=43 console_rx=44 console_rx_injection=never\n");
}

static void negative(void)
{
    clean(UART_NUM_1);
    payloads(97, 255);
    check("negative_write1", uart_write_bytes(UART_NUM_1, tx1, 129) == 129);
    api("negative_done1", uart_wait_tx_done(UART_NUM_1, WAIT));
    check("negative_no_rx1", uart_read_bytes(UART_NUM_1, rx1, 129, pdMS_TO_TICKS(100)) == 0);
#if !CONFIG_UART_NATIVE_PROFILE_ABSENT
    check("negative_no_rx2", uart_read_bytes(UART_NUM_2, rx2, 129, pdMS_TO_TICKS(100)) == 0);
    check("negative_write2", uart_write_bytes(UART_NUM_2, tx2, 129) == 129);
    api("negative_done2", uart_wait_tx_done(UART_NUM_2, WAIT));
    check("negative_no_reverse_rx1", uart_read_bytes(UART_NUM_1, rx1, 129, pdMS_TO_TICKS(100)) == 0);
#else
    check("absent_peer_not_installed", !uart_is_driver_installed(UART_NUM_2));
#endif
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("UART_NATIVE_BOOT profile=%s idf=6.1\n", profile);
    printf("UART_NATIVE_PINS tx1=17 rx1=15 tx2=18 rx2=16 rts1=11 cts1=12 rts2=13 cts2=14\n");
    /* An explicit graph pull-up makes this intentionally unconnected
     * alternate RX route electrically known idle, never a fabricated bit. */
    api("alternate_rx_input", gpio_set_direction(21, GPIO_MODE_INPUT));
    api("host_control_input", gpio_set_direction(10, GPIO_MODE_INPUT));
    if (!strcmp(profile, "uhci")) {
        failures += uart_uhci_native_run(uart0_physical);
        printf("UART_NATIVE_UNAVAILABLE ordinary_api=irda_tx_direction,irda_wctl_control rs485_autobaud_AT=available irda_receive=available arduino_idf61=not_a_locked_Arduino_profile arduino_fixture=pinned3.3.12_IDF5.5.5\n");
        printf("UART_NATIVE_DONE profile=%s failures=%u result=%s\n", profile, failures, failures ? "FAIL" : "PASS");
        return;
    }
    bool ready = setup(UART_NUM_1, TX1, RX1, RTS1, CTS1, 4096);
#if !CONFIG_UART_NATIVE_PROFILE_ABSENT
    ready = setup(UART_NUM_2, TX2, RX2, RTS2, CTS2, 4096) && ready;
#endif
    if (ready) {
        if (!strcmp(profile, "connected")) {
            transfer("duplex9600", 9600, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, 1);
            transfer("duplex115200", 115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, 7);
            transfer("duplex921600", 921600, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, 13);
            transfer("width5_even_stop1", 115200, UART_DATA_5_BITS, UART_PARITY_EVEN, UART_STOP_BITS_1, 17);
            transfer("width6_odd_stop15", 115200, UART_DATA_6_BITS, UART_PARITY_ODD, UART_STOP_BITS_1_5, 19);
            transfer("width7_even_stop2", 115200, UART_DATA_7_BITS, UART_PARITY_EVEN, UART_STOP_BITS_2, 23);
            transfer("width8_odd_stop2", 115200, UART_DATA_8_BITS, UART_PARITY_ODD, UART_STOP_BITS_2, 31);
            transfer("normal_restore", 115200, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, 37);
            threshold_timeout();
            routing_reset();
            cts_stall();
            malformed();
            at_pattern();
            autobaud();
            advanced_modes();
        } else {
            negative();
        }
    }
    if (uart_is_driver_installed(UART_NUM_1)) { api("delete1", uart_driver_delete(UART_NUM_1)); }
    if (uart_is_driver_installed(UART_NUM_2)) { api("delete2", uart_driver_delete(UART_NUM_2)); }
    uart0_physical();
    printf("UART_NATIVE_UNAVAILABLE ordinary_api=irda_tx_direction,irda_wctl_control rs485_autobaud_AT=available irda_receive=available arduino_idf61=not_a_locked_Arduino_profile arduino_fixture=pinned3.3.12_IDF5.5.5\n");
    printf("UART_NATIVE_DONE profile=%s failures=%u result=%s\n", profile, failures, failures ? "FAIL" : "PASS");
}
