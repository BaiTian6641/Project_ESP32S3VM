#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_pdm.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if ESP_IDF_VERSION_MAJOR != 6 || ESP_IDF_VERSION_MINOR != 1
#error "i2s_native requires the pinned ESP-IDF 6.1 driver"
#endif

/* IDF 6.1 uses packed 8/16/24/32-bit DMA samples on S3. No MMIO,
 * private driver API, GPIO signal-loopback, or host-provided success data. */
#define FRAMES 96
#define DESCRIPTORS 4
#define RATE 12500
#define MAX_BYTES (FRAMES * 128 / 8)
#define HASH_INIT UINT32_C(2166136261)
static uint8_t tx_data[2][MAX_BYTES];
static uint8_t rx_data[MAX_BYTES];
static uint32_t cases, failures;

typedef struct {
    volatile uint32_t sent, received, send_overflow, receive_overflow;
    volatile int64_t first_sent_us, last_sent_us;
} counters_t;
static counters_t counts[2];
static const int bclk_pin[2] = {18, 4};
static const int ws_pin[2] = {19, 5};
static const int dout_pin[2] = {20, 6};
static const int din_pin[2] = {21, 7};
static const char *formats[] = {"philips", "msb", "pcm"};

static bool sent(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel;
    (void)event;
    counters_t *c = arg;
    int64_t now = esp_timer_get_time();
    if (!c->sent) c->first_sent_us = now;
    c->last_sent_us = now;
    ++c->sent;
    return false;
}
static bool received(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel; (void)event;
    ++((counters_t *)arg)->received;
    return false;
}
static bool send_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel; (void)event;
    ++((counters_t *)arg)->send_overflow;
    return false;
}
static bool receive_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel; (void)event;
    ++((counters_t *)arg)->receive_overflow;
    return false;
}
static uint32_t sample(unsigned controller, unsigned frame, unsigned slot, unsigned bits)
{
    uint32_t v = (frame * 73 + slot * 29 + controller * 101 + 17) ^
                 ((frame + slot + 1) * UINT32_C(0x9e3779b9));
    return bits == 32 ? v : v & ((UINT32_C(1) << bits) - 1);
}
static unsigned slots_for(unsigned mask, unsigned *slots)
{
    unsigned n = 0;
    for (unsigned slot = 0; slot != 8; ++slot)
        if (mask & (1U << slot)) slots[n++] = slot;
    return n;
}
static size_t make_vector(unsigned controller, unsigned bits, unsigned mask)
{
    unsigned slots[8], n = slots_for(mask, slots);
    size_t pos = 0;
    for (unsigned f = 0; f != FRAMES; ++f)
        for (unsigned s = 0; s != n; ++s) {
            uint32_t v = sample(controller, f, slots[s], bits);
            for (unsigned b = 0; b != bits / 8; ++b)
                tx_data[controller][pos++] = v >> (8 * b);
        }
    return pos;
}
static uint32_t hash_bytes(uint32_t hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i != size; ++i) hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
}
/* Only frame-boundary phase is allowed. Every byte in the entire received
 * descriptor must match the independent transmit sequence, not a prefix. */
static int find_phase(unsigned source, unsigned bits, unsigned mask, size_t size)
{
    unsigned slots[8], channels = slots_for(mask, slots);
    size_t frame_bytes = channels * bits / 8;
    for (unsigned phase = 0; phase != FRAMES; ++phase) {
        bool equal = true;
        for (size_t i = 0; i != size; ++i) {
            if (rx_data[i] != tx_data[source][(i + phase * frame_bytes) % size]) {
                equal = false;
                break;
            }
        }
        if (equal) return phase;
    }
    return -1;
}
static uint32_t expected_hash(unsigned source, unsigned bits, unsigned mask, unsigned phase,
                              size_t size, uint32_t hash)
{
    unsigned slots[8], channels = slots_for(mask, slots);
    size_t offset = phase * channels * bits / 8;
    for (size_t i = 0; i != size; ++i)
        hash = (hash ^ tx_data[source][(i + offset) % size]) * UINT32_C(16777619);
    return hash;
}
static void check(bool ok)
{
    if (!ok) ++failures;
}
static void setup_callbacks(i2s_chan_handle_t tx, i2s_chan_handle_t rx, unsigned id)
{
    i2s_event_callbacks_t cb = {.on_sent = sent, .on_recv = received,
        .on_send_q_ovf = send_overflow, .on_recv_q_ovf = receive_overflow};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx, &cb, &counts[id]));
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx, &cb, &counts[id]));
}
static void preload(i2s_chan_handle_t tx, unsigned id, size_t size)
{
    for (unsigned d = 0; d != DESCRIPTORS; ++d) {
        size_t loaded = 0;
        ESP_ERROR_CHECK(i2s_channel_preload_data(tx, tx_data[id], size, &loaded));
        check(loaded == size);
    }
}
static void capture(unsigned case_id, const char *phase_name, i2s_chan_handle_t tx[2],
                    i2s_chan_handle_t rx[2], unsigned bits, unsigned mask, size_t size,
                    unsigned rounds, bool writes, unsigned master, unsigned frame_rate)
{
    uint32_t observed[2] = {HASH_INIT, HASH_INIT}, expected[2] = {HASH_INIT, HASH_INIT};
    unsigned bytes[2] = {0, 0}, matches[2] = {0, 0};
    int first_phase[2] = {-1, -1};
    for (unsigned round = 0; round != rounds; ++round) {
        for (unsigned id = 0; id != 2; ++id) {
            if (writes) {
                size_t written = 0;
                esp_err_t err = i2s_channel_write(tx[id], tx_data[id], size, &written, 1000);
                check(err == ESP_OK && written == size);
            }
            size_t got = 0;
            esp_err_t err = i2s_channel_read(rx[id], rx_data, size, &got, 1000);
            check(err == ESP_OK && got == size);
            bytes[id] += got;
            observed[id] = hash_bytes(observed[id], rx_data, got);
            int phase = got == size ? find_phase(1 - id, bits, mask, size) : -1;
            if (first_phase[id] < 0) first_phase[id] = phase;
            check(phase < 0 || phase == first_phase[id]);
            if (phase >= 0) {
                ++matches[id];
                expected[id] = expected_hash(1 - id, bits, mask, phase, size, expected[id]);
            } else {
                /* Never synthesize an expected hash from unmatched input. */
                expected[id] = expected_hash(1 - id, bits, mask, 0, size, expected[id]);
                check(false);
            }
        }
    }
    for (unsigned id = 0; id != 2; ++id) {
        printf("I2S_OBS case=%u phase=%s controller=%u frames=%u bytes=%u expected=%08" PRIx32
               " observed=%08" PRIx32 " match=%u/%u offset=%d txeof=%" PRIu32
               " rxeof=%" PRIu32 " txovf=%" PRIu32 " rxovf=%" PRIu32 "\n",
               case_id, phase_name, id, rounds * FRAMES, bytes[id], expected[id], observed[id],
               matches[id], rounds, first_phase[id], counts[id].sent, counts[id].received,
               counts[id].send_overflow, counts[id].receive_overflow);
    }
    counters_t *c = &counts[master];
    printf("I2S_CADENCE case=%u phase=%s frame_hz=%u/1 interval_ns=%u/1"
           " eof_frames=%u intervals=%" PRIu32 " elapsed_us=%" PRId64 "\n",
           case_id, phase_name, frame_rate, 1000000000U / frame_rate, FRAMES,
           c->sent ? c->sent - 1 : 0, c->last_sent_us - c->first_sent_us);
    check(c->sent >= rounds);
}
static void start_pair(i2s_chan_handle_t tx[2], i2s_chan_handle_t rx[2], unsigned master)
{
    unsigned slave = 1 - master;
    ESP_ERROR_CHECK(i2s_channel_enable(rx[slave]));
    ESP_ERROR_CHECK(i2s_channel_enable(tx[slave]));
    ESP_ERROR_CHECK(i2s_channel_enable(rx[master]));
    ESP_ERROR_CHECK(i2s_channel_enable(tx[master]));
}
static void stop_pair(i2s_chan_handle_t tx[2], i2s_chan_handle_t rx[2], unsigned master)
{
    ESP_ERROR_CHECK(i2s_channel_disable(tx[master]));
    ESP_ERROR_CHECK(i2s_channel_disable(rx[master]));
    ESP_ERROR_CHECK(i2s_channel_disable(tx[1 - master]));
    ESP_ERROR_CHECK(i2s_channel_disable(rx[1 - master]));
}
static void run_case(bool tdm, unsigned master, unsigned format, unsigned bits, unsigned mask,
                     bool mono)
{
    unsigned case_id = cases++;
    i2s_chan_handle_t tx[2], rx[2];
    memset(counts, 0, sizeof(counts));
    size_t size = make_vector(0, bits, mask);
    make_vector(1, bits, mask);
    printf("I2S_CASE case=%u mode=%s format=%s bits=%u mask=%02x mono=%u master=%u rate=%u\n",
           case_id, tdm ? "tdm" : "std", formats[format], bits, mask, mono, master, RATE);
    for (unsigned id = 0; id != 2; ++id) {
        i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(id,
            id == master ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
        chan.dma_desc_num = DESCRIPTORS;
        chan.dma_frame_num = FRAMES;
        ESP_ERROR_CHECK(i2s_new_channel(&chan, &tx[id], &rx[id]));
        i2s_slot_mode_t mode = mono ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
        if (tdm) {
            i2s_tdm_config_t config = {
                .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(RATE),
                .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode, mask),
                .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = bclk_pin[id], .ws = ws_pin[id],
                             .dout = dout_pin[id], .din = din_pin[id]},
            };
            if (format == 1) config.slot_cfg = (i2s_tdm_slot_config_t)
                I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(bits, mode, mask);
            if (format == 2) config.slot_cfg = (i2s_tdm_slot_config_t)
                I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(bits, mode, mask);
            config.slot_cfg.total_slot = bits <= 16 ? 8 : 4;
            config.slot_cfg.skip_mask = false;
            config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_768;
            ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(tx[id], &config));
            ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx[id], &config));
        } else {
            i2s_std_config_t config = {
                .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode),
                .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = bclk_pin[id], .ws = ws_pin[id],
                             .dout = dout_pin[id], .din = din_pin[id]},
            };
            if (format == 1) config.slot_cfg = (i2s_std_slot_config_t)
                I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits, mode);
            if (format == 2) config.slot_cfg = (i2s_std_slot_config_t)
                I2S_STD_PCM_SLOT_DEFAULT_CONFIG(bits, mode);
            config.slot_cfg.slot_mask = mono ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH;
            if (bits == 24) config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
            ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx[id], &config));
            ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx[id], &config));
        }
        setup_callbacks(tx[id], rx[id], id);
        preload(tx[id], id, size);
    }
    start_pair(tx, rx, master);
    /* Full-duplex driver does not promise synchronous starts: discard one
     * initial descriptor, then require complete periodic frame agreement. */
    for (unsigned id = 0; id != 2; ++id) {
        size_t got = 0;
        ESP_ERROR_CHECK(i2s_channel_read(rx[id], rx_data, size, &got, 1000));
        check(got == size);
    }
    capture(case_id, "sustained", tx, rx, bits, mask, size, 8, true, master, RATE);
    uint32_t before = counts[master].sent;
    uint32_t overflow_before = counts[master].receive_overflow;
    /* No producer/read activity for more than two complete descriptor rings.
     * With IDF auto-clear disabled, DMA legitimately repeats its last contents. */
    vTaskDelay(pdMS_TO_TICKS(80));
    check(counts[master].sent > before + DESCRIPTORS);
    check(counts[master].receive_overflow > overflow_before);
    capture(case_id, "starved", tx, rx, bits, mask, size, 4, false, master, RATE);
    stop_pair(tx, rx, master);
    uint32_t stopped[2] = {counts[0].sent, counts[1].sent};
    vTaskDelay(pdMS_TO_TICKS(2));
    bool frozen = stopped[0] == counts[0].sent && stopped[1] == counts[1].sent;
    check(frozen);
    for (unsigned id = 0; id != 2; ++id) {
        size_t moved = 0;
        esp_err_t read_err = i2s_channel_read(rx[id], rx_data, size, &moved, 0);
        esp_err_t write_err = i2s_channel_write(tx[id], tx_data[id], size, &moved, 0);
        printf("I2S_DISABLED case=%u controller=%u read=%d write=%d frozen=%u\n",
               case_id, id, read_err, write_err, frozen);
        check(read_err == ESP_ERR_INVALID_STATE && write_err == ESP_ERR_INVALID_STATE);
        preload(tx[id], id, size);
    }
    /* Restart cadence must not include the intentional disabled interval. */
    memset(counts, 0, sizeof(counts));
    start_pair(tx, rx, master);
    capture(case_id, "restart", tx, rx, bits, mask, size, 4, true, master, RATE);
    stop_pair(tx, rx, master);
    for (unsigned id = 0; id != 2; ++id) {
        ESP_ERROR_CHECK(i2s_del_channel(tx[id]));
        ESP_ERROR_CHECK(i2s_del_channel(rx[id]));
    }
}
static void pdm_capabilities(void)
{
    for (unsigned id = 0; id != 2; ++id) {
        for (unsigned pcm = 0; pcm != 2; ++pcm) {
            for (unsigned direction = 0; direction != 2; ++direction) {
                i2s_chan_handle_t channel;
                i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(id, I2S_ROLE_MASTER);
                chan.dma_desc_num = DESCRIPTORS;
                chan.dma_frame_num = FRAMES;
                ESP_ERROR_CHECK(i2s_new_channel(&chan, direction ? NULL : &channel,
                                                direction ? &channel : NULL));
                esp_err_t err;
                if (direction) {
                    i2s_pdm_rx_config_t config = {
                        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(pcm ? 16000 : 1000000),
                        .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
                        .gpio_cfg = {.clk = bclk_pin[id], .din = din_pin[id]},
                    };
                    if (pcm) config.slot_cfg.data_fmt = I2S_PDM_DATA_FMT_PCM;
                    err = i2s_channel_init_pdm_rx_mode(channel, &config);
                } else {
                    i2s_pdm_tx_config_t config = {
                        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(pcm ? 16000 : 1000000),
                        .slot_cfg = I2S_PDM_TX_SLOT_RAW_FMT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
                        .gpio_cfg = {.clk = bclk_pin[id], .dout = dout_pin[id],
                                     .dout2 = I2S_GPIO_UNUSED},
                    };
                    if (pcm) config.slot_cfg.data_fmt = I2S_PDM_DATA_FMT_PCM;
                    err = i2s_channel_init_pdm_tx_mode(channel, &config);
                }
                esp_err_t expected = pcm && id == 1 ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
                printf("I2S_CAP controller=%u direction=%s format=%s expected=%d observed=%d\n",
                       id, direction ? "rx" : "tx", pcm ? "pcm" : "raw", expected, err);
                check(err == expected);
                ESP_ERROR_CHECK(i2s_del_channel(channel));
            }
        }
    }
}
static void raw_pdm(unsigned source, unsigned master)
{
    unsigned destination = 1 - source, case_id = cases++;
    i2s_chan_handle_t tx, rx;
    memset(counts, 0, sizeof(counts));
    size_t size = make_vector(source, 16, 3);
    printf("I2S_CASE case=%u mode=pdm format=raw bits=16 mask=03 mono=0 master=%u rate=1000000 source=%u\n",
           case_id, master, source);
    i2s_chan_config_t tchan = I2S_CHANNEL_DEFAULT_CONFIG(source,
        source == master ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    i2s_chan_config_t rchan = I2S_CHANNEL_DEFAULT_CONFIG(destination,
        destination == master ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    tchan.dma_desc_num = rchan.dma_desc_num = DESCRIPTORS;
    tchan.dma_frame_num = rchan.dma_frame_num = FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&tchan, &tx, NULL));
    ESP_ERROR_CHECK(i2s_new_channel(&rchan, NULL, &rx));
    i2s_pdm_tx_config_t tcfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(1000000),
        .slot_cfg = I2S_PDM_TX_SLOT_RAW_FMT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.clk = bclk_pin[source], .dout = dout_pin[source],
                     .dout2 = I2S_GPIO_UNUSED},
    };
    i2s_pdm_rx_config_t rcfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(1000000),
        .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.clk = bclk_pin[destination], .din = din_pin[destination]},
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(tx, &tcfg));
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx, &rcfg));
    i2s_event_callbacks_t cb = {.on_sent = sent, .on_recv = received,
        .on_send_q_ovf = send_overflow, .on_recv_q_ovf = receive_overflow};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx, &cb, &counts[source]));
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx, &cb, &counts[destination]));
    preload(tx, source, size);
    if (source == master) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx));
        ESP_ERROR_CHECK(i2s_channel_enable(tx));
    } else {
        ESP_ERROR_CHECK(i2s_channel_enable(tx));
        ESP_ERROR_CHECK(i2s_channel_enable(rx));
    }
    size_t moved = 0;
    ESP_ERROR_CHECK(i2s_channel_read(rx, rx_data, size, &moved, 1000));
    check(moved == size);
    uint32_t expected = HASH_INIT, observed = HASH_INIT;
    unsigned matches = 0, bytes = 0;
    int offset = -1;
    for (unsigned round = 0; round != 8; ++round) {
        moved = 0;
        esp_err_t err = i2s_channel_write(tx, tx_data[source], size, &moved, 1000);
        check(err == ESP_OK && moved == size);
        moved = 0;
        err = i2s_channel_read(rx, rx_data, size, &moved, 1000);
        check(err == ESP_OK && moved == size);
        bytes += moved;
        observed = hash_bytes(observed, rx_data, moved);
        int phase = moved == size ? find_phase(source, 16, 3, size) : -1;
        if (round == 0) offset = phase;
        check(phase >= 0 && phase == offset);
        if (phase >= 0) ++matches;
        expected = expected_hash(source, 16, 3, phase < 0 ? 0 : phase, size, expected);
    }
    printf("I2S_OBS case=%u phase=sustained controller=%u frames=768 bytes=%u expected=%08" PRIx32
           " observed=%08" PRIx32 " match=%u/8 offset=%d txeof=%" PRIu32
           " rxeof=%" PRIu32 " txovf=%" PRIu32 " rxovf=%" PRIu32 "\n",
           case_id, destination, bytes, expected, observed, matches, offset, counts[source].sent,
           counts[destination].received, counts[source].send_overflow, counts[destination].receive_overflow);
    counters_t *c = &counts[source];
    printf("I2S_CADENCE case=%u phase=sustained frame_hz=62500/1 interval_ns=16000/1"
           " eof_frames=96 intervals=%" PRIu32 " elapsed_us=%" PRId64 "\n",
           case_id, c->sent ? c->sent - 1 : 0, c->last_sent_us - c->first_sent_us);
    check(c->sent >= 8 && counts[destination].received >= 8);
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
    ESP_ERROR_CHECK(i2s_channel_disable(rx));
    ESP_ERROR_CHECK(i2s_del_channel(tx));
    ESP_ERROR_CHECK(i2s_del_channel(rx));
}

static void tdm_frame_limits(void)
{
    /* S3 half-frame field caps the physical frame at 128 bits. Never
     * truncate wider eight-slot configurations to make them look supported. */
    for (unsigned id = 0; id != 2; ++id)
        for (unsigned bits = 24; bits <= 32; bits += 8)
            for (unsigned direction = 0; direction != 2; ++direction) {
                i2s_chan_handle_t channel;
                i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(id, I2S_ROLE_MASTER);
                ESP_ERROR_CHECK(i2s_new_channel(&chan, direction ? NULL : &channel,
                                                direction ? &channel : NULL));
                i2s_tdm_config_t config = {
                    .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(RATE),
                    .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO, 0xff),
                    .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = bclk_pin[id], .ws = ws_pin[id],
                                 .dout = dout_pin[id], .din = din_pin[id]},
                };
                config.slot_cfg.total_slot = 8;
                esp_err_t err = i2s_channel_init_tdm_mode(channel, &config);
                printf("I2S_TDM_LIMIT controller=%u direction=%s bits=%u slots=8 expected=%d observed=%d\n",
                       id, direction ? "rx" : "tx", bits, ESP_ERR_INVALID_ARG, err);
                check(err == ESP_ERR_INVALID_ARG);
                ESP_ERROR_CHECK(i2s_del_channel(channel));
            }
}

void app_main(void)
{
    printf("I2S_NATIVE_READY version=1 packed=1 descriptors=%u frames=%u\n", DESCRIPTORS, FRAMES);
    const unsigned widths[] = {8, 16, 24, 32};
    const unsigned masks[] = {0x81, 0x55, 0xff};
    const unsigned wide_masks[] = {0x09, 0x05, 0x0f};
    for (unsigned master = 0; master != 2; ++master)
        for (unsigned format = 0; format != 3; ++format)
            for (unsigned width = 0; width != 4; ++width)
                for (unsigned mono = 0; mono != 2; ++mono)
                    run_case(false, master, format, widths[width], mono ? 1 : 3, mono);
    for (unsigned master = 0; master != 2; ++master)
        for (unsigned format = 0; format != 3; ++format)
            for (unsigned width = 0; width != 4; ++width)
                for (unsigned mask = 0; mask != 3; ++mask)
                    run_case(true, master, format, widths[width],
                             widths[width] <= 16 ? masks[mask] : wide_masks[mask], false);
    for (unsigned source = 0; source != 2; ++source)
        for (unsigned master = 0; master != 2; ++master)
            raw_pdm(source, master);
    pdm_capabilities();
    tdm_frame_limits();
    printf("I2S_UNQUALIFIED mode=pcm2pdm,pdm2pcm reason=no-independent-converter-reference\n");
    printf("I2S_UNQUALIFIED mode=external-peer reason=no-registered-I2S-peer-schema\n");
    printf("I2S_NATIVE_DONE cases=%" PRIu32 " failures=%" PRIu32 "\n", cases, failures);
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
