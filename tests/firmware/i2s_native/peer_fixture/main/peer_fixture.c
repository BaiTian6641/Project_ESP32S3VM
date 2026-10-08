#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_pdm.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if ESP_IDF_VERSION_MAJOR != 6 || ESP_IDF_VERSION_MINOR != 1
#error "The external peer fixture requires pinned ESP-IDF 6.1"
#endif
typedef struct {
    const char *name;
    unsigned format, bits, slots, mask, master, controller;
} peer_case_t;
#include "peer_cases.h"
static const peer_case_t *selected;
static unsigned case_id;
#define PEER_FORMAT_ID (selected->format)
#define PEER_BITS (selected->bits)
#define PEER_SLOTS (selected->slots)
#define PEER_MASK (selected->mask)
#define PEER_MASTER (selected->master)
#define PEER_CONTROLLER (selected->controller)
#define FRAMES 96U
#define DESCRIPTORS 4U
#define ROUNDS 8U
#define MAX_BYTES (FRAMES * 128 / 8)
#define HASH_INIT UINT32_C(2166136261)
static uint8_t tx_data[MAX_BYTES], expected_data[MAX_BYTES], rx_data[MAX_BYTES];
static unsigned failures;
static volatile uint32_t tx_eof, rx_eof, tx_overflow, rx_overflow;
static volatile int64_t first_eof_us, last_eof_us;
static const char *const formats[] = {"philips", "msb", "pcm", "tdm", "raw-pdm"};

static uint32_t sample(unsigned source, unsigned frame, unsigned slot)
{
    uint32_t value = (frame * 73 + slot * 29 + source * 101 + 17) ^
                     ((frame + slot + 1) * UINT32_C(0x9e3779b9));
    if (PEER_BITS != 32)
        value &= (UINT32_C(1) << PEER_BITS) - 1;
    return value;
}
static size_t vector(uint8_t *data, unsigned source)
{
    size_t size = 0;
    for (unsigned frame = 0; frame < FRAMES; ++frame)
        for (unsigned slot = 0; slot < PEER_SLOTS; ++slot)
            if (PEER_MASK & (1U << slot)) {
                uint32_t value = sample(source, frame, slot);
                for (unsigned byte = 0; byte < PEER_BITS / 8; ++byte)
                    data[size++] = value >> (byte * 8);
            }
    return size;
}
static uint32_t hash(uint32_t value, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        value = (value ^ data[i]) * UINT32_C(16777619);
    return value;
}
static bool on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *arg)
{
    (void)handle; (void)event; (void)arg;
    int64_t now = esp_timer_get_time();
    if (!tx_eof) first_eof_us = now;
    last_eof_us = now;
    ++tx_eof;
    return false;
}
static bool on_recv(i2s_chan_handle_t handle, i2s_event_data_t *event, void *arg)
{
    (void)handle; (void)event; (void)arg; ++rx_eof; return false;
}
static bool on_tx_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *arg)
{
    (void)handle; (void)event; (void)arg; ++tx_overflow; return false;
}
static bool on_rx_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *arg)
{
    (void)handle; (void)event; (void)arg; ++rx_overflow; return false;
}
static void init_mode(i2s_chan_handle_t tx, i2s_chan_handle_t rx)
{
    if (PEER_FORMAT_ID == 4) {
    i2s_pdm_tx_config_t t = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(1000000),
        .slot_cfg = I2S_PDM_TX_SLOT_RAW_FMT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.clk = 18, .dout = 20, .dout2 = I2S_GPIO_UNUSED},
    };
    i2s_pdm_rx_config_t r = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(1000000),
        .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.clk = 18, .din = 21},
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(tx, &t));
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx, &r));
    } else if (PEER_FORMAT_ID == 3) {
    i2s_tdm_config_t config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(12500),
        .slot_cfg = I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(PEER_BITS, I2S_SLOT_MODE_STEREO, PEER_MASK),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = 18, .ws = 19, .dout = 20, .din = 21},
    };
    config.slot_cfg.total_slot = PEER_SLOTS;
    config.slot_cfg.skip_mask = false;
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_768;
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(tx, &config));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &config));
    } else {
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(12500),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(PEER_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = 18, .ws = 19, .dout = 20, .din = 21},
    };
    if (PEER_FORMAT_ID == 1)
        config.slot_cfg = (i2s_std_slot_config_t)I2S_STD_MSB_SLOT_DEFAULT_CONFIG(PEER_BITS, I2S_SLOT_MODE_STEREO);
    else if (PEER_FORMAT_ID == 2)
        config.slot_cfg = (i2s_std_slot_config_t)I2S_STD_PCM_SLOT_DEFAULT_CONFIG(PEER_BITS, I2S_SLOT_MODE_STEREO);
    config.slot_cfg.slot_mask = PEER_MASK;
    if (PEER_BITS == 24)
        config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &config));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &config));
    }
}
/* Match every byte of an independent source descriptor; only a complete
 * frame rotation is accepted, never a prefix, masked bit or echo stream. */
static int find_phase(size_t size)
{
    size_t frame_bytes = size / FRAMES;
    for (unsigned phase = 0; phase < FRAMES; ++phase) {
        bool equal = true;
        for (size_t i = 0; i < size; ++i)
            if (rx_data[i] != expected_data[(i + phase * frame_bytes) % size]) {
                equal = false;
                break;
            }
        if (equal) return phase;
    }
    return -1;
}
void app_main(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    printf("I2S_PEER_SELECT cases=%u protocol=CASE-zero-based\n",
           (unsigned)(sizeof(cases) / sizeof(cases[0])));
    for (;;) {
        char line[64];
        size_t used = 0;
        bool overflow = false;
        uint8_t byte = 0;
        do {
            if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1)
                continue;
            if (byte != '\r' && byte != '\n') {
                if (used + 1 < sizeof(line)) line[used++] = byte;
                else overflow = true;
            }
        } while (byte != '\n');
        line[used] = '\0';
        unsigned id;
        char extra;
        if (!overflow && sscanf(line, "CASE %u %c", &id, &extra) == 1 &&
            id < sizeof(cases) / sizeof(cases[0])) {
            case_id = id;
            selected = &cases[id];
            printf("I2S_PEER_CASE id=%u name=%s\n", id, selected->name);
            break;
        }
        printf("I2S_PEER_CASE_ERROR invalid-command\n");
    }
    size_t size = vector(tx_data, 0);
    vector(expected_data, 1);
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(PEER_CONTROLLER,
                              PEER_MASTER ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    channel.dma_desc_num = DESCRIPTORS;
    channel.dma_frame_num = FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &tx, &rx));
    init_mode(tx, rx);
    i2s_event_callbacks_t callbacks = {.on_sent = on_sent, .on_recv = on_recv,
        .on_send_q_ovf = on_tx_overflow, .on_recv_q_ovf = on_rx_overflow};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx, &callbacks, NULL));
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx, &callbacks, NULL));
    for (unsigned descriptor = 0; descriptor < DESCRIPTORS; ++descriptor) {
        size_t loaded = 0;
        ESP_ERROR_CHECK(i2s_channel_preload_data(tx, tx_data, size, &loaded));
        if (loaded != size) ++failures;
    }
    /* Lifecycle handshake only: this byte carries no samples/results. Apply
     * the real graph after READY while the VM is stopped, then queue G and
     * continue. A peer master cannot free-run through the entire IDF boot. */
    printf("I2S_PEER_READY id=%u name=%s format=%s bits=%u slots=%u mask=%x master=%u controller=%u bytes=%u\n",
           case_id, selected->name, formats[PEER_FORMAT_ID], PEER_BITS, PEER_SLOTS, PEER_MASK, PEER_MASTER, PEER_CONTROLLER, (unsigned)size);
    uint8_t command = 0;
    do {
        uart_read_bytes(UART_NUM_0, &command, 1, portMAX_DELAY);
    } while (command != 'G');
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    size_t got = 0;
    ESP_ERROR_CHECK(i2s_channel_read(rx, rx_data, size, &got, 1000));
    if (got != size) ++failures;
    uint32_t observed = HASH_INIT, expected = HASH_INIT;
    unsigned matches = 0, bytes = 0;
    int phase = -1;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        size_t written = 0;
        esp_err_t result = i2s_channel_write(tx, tx_data, size, &written, 1000);
        if (result != ESP_OK || written != size) ++failures;
        got = 0;
        result = i2s_channel_read(rx, rx_data, size, &got, 1000);
        if (result != ESP_OK || got != size) ++failures;
        observed = hash(observed, rx_data, got);
        bytes += got;
        int found = got == size ? find_phase(size) : -1;
        if (!round) phase = found;
        if (found >= 0 && found == phase) ++matches; else ++failures;
        size_t offset = (phase < 0 ? 0 : phase) * (size / FRAMES);
        for (size_t i = 0; i < size; ++i)
            expected = (expected ^ expected_data[(i + offset) % size]) * UINT32_C(16777619);
    }
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
    ESP_ERROR_CHECK(i2s_channel_disable(rx));
    if (tx_eof < ROUNDS || rx_eof < ROUNDS || expected != observed) ++failures;
    printf("I2S_PEER_OBS frames=%u bytes=%u expected=%08" PRIx32 " observed=%08" PRIx32
           " match=%u/%u offset=%d txeof=%" PRIu32 " rxeof=%" PRIu32
           " txovf=%" PRIu32 " rxovf=%" PRIu32 "\n",
           ROUNDS * FRAMES, bytes, expected, observed, matches, ROUNDS, phase,
           tx_eof, rx_eof, tx_overflow, rx_overflow);
    printf("I2S_PEER_CADENCE frame_hz=%u/1 eof_frames=%u intervals=%" PRIu32 " elapsed_us=%" PRId64 "\n",
           PEER_FORMAT_ID == 4 ? 62500 : 12500, FRAMES, tx_eof ? tx_eof - 1 : 0, last_eof_us - first_eof_us);
    printf("I2S_PEER_DONE failures=%u\n", failures);
    ESP_ERROR_CHECK(i2s_del_channel(tx));
    ESP_ERROR_CHECK(i2s_del_channel(rx));
    for (;;) vTaskDelay(portMAX_DELAY);
}
