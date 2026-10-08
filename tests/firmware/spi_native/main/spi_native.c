/* SPDX-License-Identifier: Apache-2.0
 * Only the host's real v3 electrical graph supplies wires and NOR devices.
 * Ordinary IDF APIs own PIO/queued GDMA. Raw MMIO is confined to cancellation
 * cases that spi_master cannot express; it never injects incoming SPI data.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "hal/dma_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/spi_reg.h"
#include "soc/system_reg.h"
#include "soc/gdma_reg.h"

#define LARGE_BYTES 8201
#define STORAGE_BYTES ((LARGE_BYTES + 3) & ~3)
#define NOR_BASE 0x2000
#define WAIT_TICKS pdMS_TO_TICKS(1000)
static const int pins[2][5] = {{12, 11, 13, 10, 9}, {36, 35, 37, 34, 33}};
static unsigned failures;
static uintptr_t spi_base(unsigned host) { return host == 2 ? 0x60024000 : 0x60025000; }
static void check(unsigned host, const char *name, bool ok)
{
    printf("SPI_NATIVE_CHECK host=%u name=%s result=%s\n", host, name, ok ? "PASS" : "FAIL");
    failures += !ok;
    fflush(stdout);
}
static bool status(unsigned host, const char *name, esp_err_t actual)
{
    printf("SPI_NATIVE_STATUS host=%u name=%s actual=%s\n", host, name, esp_err_to_name(actual));
    check(host, name, actual == ESP_OK);
    return actual == ESP_OK;
}
static uint32_t hash(const uint8_t *bytes, size_t size)
{
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}
static void payload(unsigned host, const char *name, const uint8_t *data, size_t size)
{
    printf("SPI_NATIVE_RX host=%u name=%s bytes=%u fnv1a=%08" PRIx32 " data=", host, name, (unsigned)size, hash(data, size));
    /* Large transfers include every byte: hashes alone do not qualify equality. */
    for (size_t i = 0; i < size; ++i) printf("%02x", data[i]);
    printf("\n");
    fflush(stdout);
}
static bool erased(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) if (data[i] != 0xff) return false;
    return true;
}
static bool bus_init(unsigned host, bool dma)
{
    const int *p = pins[host - 2];
    printf("SPI_NATIVE_PHASE host=%u engine=%s sclk=%d mosi=%d miso=%d cs0=%d cs1=%d speed_hz=%d route=matrix virtual_us=%" PRId64 "\n",
           host, dma ? "GDMA" : "PIO", p[0], p[1], p[2], p[3], p[4], CONFIG_SPI_NATIVE_SPEED_HZ, esp_timer_get_time());
    fflush(stdout);
    spi_bus_config_t config = {
        .sclk_io_num = p[0], .mosi_io_num = p[1], .miso_io_num = p[2],
        .quadwp_io_num = -1, .quadhd_io_num = -1, .data4_io_num = -1,
        .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = dma ? LARGE_BYTES : 64,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    return status(host, "bus_init", spi_bus_initialize((spi_host_device_t)(host - 1), &config, dma ? SPI_DMA_CH_AUTO : SPI_DMA_DISABLED));
}

/* Snapshot the ordinary driver's actual three-word GDMA descriptors in its
 * callbacks. No private driver structures, extra IRQ or favorable read map. */
typedef struct { uint32_t control, buffer, next; } descriptor;
typedef struct {
    unsigned host, pre_count, post_count;
    int rx_channel, tx_channel;
    uintptr_t rx_head, tx_head;
    uint32_t rx_before[4], rx_after[4], tx_before[4], tx_after[4];
    unsigned rx_count, tx_count;
    uint32_t spi_raw, spi_masked, rx_raw, tx_raw, rx_eof, tx_eof, tx_config;
    int64_t start_us, end_us;
} event;
static bool descriptor_address(uintptr_t address)
{
    return address >= 0x3fc88000 && address <= 0x3fd00000 - sizeof(descriptor) && !(address & 3);
}
static unsigned descriptors(uintptr_t address, uint32_t values[4])
{
    unsigned count = 0;
    while (count < 4 && descriptor_address(address)) {
        volatile descriptor *d = (volatile descriptor *)address;
        values[count++] = d->control;
        address = d->next;
    }
    return count;
}
static void pre_callback(spi_transaction_t *transaction)
{
    event *e = transaction->user;
    if (!e) return;
    e->pre_count++;
    e->start_us = esp_timer_get_time();
    e->rx_channel = e->tx_channel = -1;
    for (unsigned ch = 0; ch < 5; ++ch) {
        uintptr_t stride = ch * 0xc0;
        /* SPI2's GDMA peri_sel is 0, colliding with the PERI_SEL reset value:
         * require an actually armed link so reset channels never match. */
        uint32_t in_armed = REG_READ(GDMA_IN_LINK_CH0_REG + stride) & 0xfffff;
        uint32_t out_armed = REG_READ(GDMA_OUT_LINK_CH0_REG + stride) & 0xfffff;
        if (in_armed && (REG_READ(GDMA_IN_PERI_SEL_CH0_REG + stride) & 63) == e->host - 2) e->rx_channel = ch;
        if (out_armed && (REG_READ(GDMA_OUT_PERI_SEL_CH0_REG + stride) & 63) == e->host - 2) e->tx_channel = ch;
    }
    if (e->rx_channel >= 0) {
        e->rx_head = 0x3fc00000 | (REG_READ(GDMA_IN_LINK_CH0_REG + e->rx_channel * 0xc0) & 0xfffff);
        e->rx_count = descriptors(e->rx_head, e->rx_before);
    }
    if (e->tx_channel >= 0) {
        e->tx_head = 0x3fc00000 | (REG_READ(GDMA_OUT_LINK_CH0_REG + e->tx_channel * 0xc0) & 0xfffff);
        e->tx_count = descriptors(e->tx_head, e->tx_before);
        e->tx_config = REG_READ(GDMA_OUT_CONF0_CH0_REG + e->tx_channel * 0xc0);
    }
}
static void post_callback(spi_transaction_t *transaction)
{
    event *e = transaction->user;
    if (!e) return;
    e->post_count++;
    e->end_us = esp_timer_get_time();
    e->spi_raw = REG_READ(spi_base(e->host) + 0x3c);
    e->spi_masked = REG_READ(spi_base(e->host) + 0x40);
    if (e->rx_channel >= 0) {
        e->rx_raw = REG_READ(GDMA_IN_INT_RAW_CH0_REG + e->rx_channel * 0xc0);
        e->rx_eof = REG_READ(GDMA_IN_SUC_EOF_DES_ADDR_CH0_REG + e->rx_channel * 0xc0);
        descriptors(e->rx_head, e->rx_after);
    }
    if (e->tx_channel >= 0) {
        e->tx_raw = REG_READ(GDMA_OUT_INT_RAW_CH0_REG + e->tx_channel * 0xc0);
        e->tx_eof = REG_READ(GDMA_OUT_EOF_DES_ADDR_CH0_REG + e->tx_channel * 0xc0);
        descriptors(e->tx_head, e->tx_after);
    }
}
static void report_event(event *e, const char *name, bool receive, bool transmit, size_t size)
{
    printf("SPI_NATIVE_EVENT host=%u name=%s pre=%u post=%u start_us=%" PRId64 " end_us=%" PRId64 " spi_raw=%08" PRIx32 " spi_masked=%08" PRIx32 " rx_ch=%d tx_ch=%d rx_raw=%08" PRIx32 " tx_raw=%08" PRIx32 " rx_eof=%08" PRIx32 " tx_eof=%08" PRIx32 " tx_conf=%08" PRIx32 "\n",
           e->host, name, e->pre_count, e->post_count, e->start_us, e->end_us, e->spi_raw, e->spi_masked, e->rx_channel, e->tx_channel, e->rx_raw, e->tx_raw, e->rx_eof, e->tx_eof, e->tx_config);
    bool good = e->pre_count == 1 && e->post_count == 1 && e->end_us > e->start_us && (e->spi_raw & SPI_TRANS_DONE_INT_RAW);
    unsigned count[2] = {e->rx_count, e->tx_count};
    size_t total[2] = {0, 0};
    uintptr_t last[2] = {0, 0};
    for (unsigned direction = 0; direction < 2; ++direction) {
        bool enabled = direction ? transmit : receive;
        if (!enabled) continue;
        uint32_t *before = direction ? e->tx_before : e->rx_before;
        uint32_t *after = direction ? e->tx_after : e->rx_after;
        uintptr_t address = direction ? e->tx_head : e->rx_head;
        for (unsigned i = 0; i < count[direction]; ++i) {
            printf("SPI_NATIVE_DESCRIPTOR host=%u name=%s direction=%s index=%u address=%08" PRIxPTR " before=%08" PRIx32 " after=%08" PRIx32 "\n", e->host, name, direction ? "TX" : "RX", i, address, before[i], after[i]);
            good &= !!(before[i] & (1u << 31));
            total[direction] += (after[i] >> 12) & 4095;
            if (!direction || (e->tx_config & GDMA_OUT_AUTO_WRBACK_CH0)) good &= !(after[i] & (1u << 31));
            if (!direction) good &= !!(after[i] & BIT(30)) == (i + 1 == count[direction]);
            last[direction] = address;
            if (descriptor_address(address)) address = ((volatile descriptor *)address)->next;
        }
        size_t minimum = (size + DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED - 1) /
                         DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED;
        good &= count[direction] >= minimum && total[direction] == size;
    }
    if (receive) good &= (e->rx_raw & GDMA_IN_SUC_EOF_CH0_INT_RAW) && e->rx_eof == last[0];
    if (transmit) good &= (e->tx_raw & GDMA_OUT_TOTAL_EOF_CH0_INT_RAW) && e->tx_eof == last[1];
    check(e->host, name, good);
}
static spi_device_handle_t add_device(unsigned host, unsigned cs, unsigned mode, bool half, unsigned order, bool callbacks)
{
    spi_device_interface_config_t config = {
        .clock_speed_hz = CONFIG_SPI_NATIVE_SPEED_HZ, .mode = mode,
        .spics_io_num = pins[host - 2][3 + cs], .queue_size = 3,
        .command_bits = half ? 8 : 0,
        .flags = (half ? SPI_DEVICE_HALFDUPLEX : 0) | SPI_DEVICE_NO_DUMMY |
                 ((order == 1 || order == 2) ? SPI_DEVICE_TXBIT_LSBFIRST : 0) |
                 ((order == 1 || order == 3) ? SPI_DEVICE_RXBIT_LSBFIRST : 0),
        .pre_cb = callbacks ? pre_callback : NULL, .post_cb = callbacks ? post_callback : NULL,
    };
    spi_device_handle_t device = NULL;
    status(host, "add_device", spi_bus_add_device((spi_host_device_t)(host - 1), &config, &device));
    return device;
}
static bool nor_transfer(unsigned host, spi_device_handle_t device, uint8_t command, uint32_t address,
                         bool addressed, const void *tx, void *rx, size_t size, bool queued, event *e)
{
    spi_transaction_ext_t t = {
        .base = {.flags = SPI_TRANS_VARIABLE_ADDR, .cmd = command, .addr = address,
                 .length = tx ? size * 8 : 0, .rxlength = rx ? size * 8 : 0,
                 .tx_buffer = tx, .rx_buffer = rx, .user = e},
        .address_bits = addressed ? 24 : 0,
    };
    if (!queued) {
        esp_err_t result = spi_device_polling_transmit(device, &t.base);
        /* Do not spend virtual UART time logging between program/erase and
         * the first status sample. Errors remain explicit fixture failures. */
        return result == ESP_OK || status(host, "nor_polling", result);
    }
    if (!status(host, "nor_queue", spi_device_queue_trans(device, &t.base, WAIT_TICKS))) return false;
    spi_transaction_t *done = NULL;
    if (!status(host, "nor_result", spi_device_get_trans_result(device, &done, WAIT_TICKS))) return false;
    check(host, "nor_result_identity", done == &t.base);
    return done == &t.base;
}
static uint8_t nor_status(unsigned host, spi_device_handle_t device)
{
    uint8_t result = 0xa5;
    nor_transfer(host, device, 0x05, 0, false, NULL, &result, 1, false, NULL);
    return result;
}
static bool nor_wait(unsigned host, spi_device_handle_t device, const char *name)
{
    int64_t start = esp_timer_get_time();
    unsigned polls = 0;
    uint8_t first_status = 0;
    uint8_t value;
    do {
        value = nor_status(host, device);
        polls++;
        if (polls == 1) first_status = value;
        if (!(value & 1)) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() - start < 100000);
    printf("SPI_NATIVE_BUSY host=%u name=%s polls=%u elapsed_us=%" PRId64 " first_status=%02x final=%02x\n", host, name, polls, esp_timer_get_time() - start, first_status, value);
    check(host, name, !(value & 3));
    return !(value & 3);
}
static void nor_wren(unsigned host, spi_device_handle_t device)
{
    nor_transfer(host, device, 0x06, 0, false, NULL, NULL, 0, false, NULL);
    check(host, "wren_wel", (nor_status(host, device) & 3) == 2);
}
static void nor_pio(unsigned host)
{
    if (!bus_init(host, false)) return;
    spi_device_handle_t a = add_device(host, 0, 0, true, false, false);
    spi_device_handle_t b = add_device(host, 1, 3, true, false, false);
    if (a && b) {
        uint8_t id[3] = {0}, wanted[48], got[48] = {0};
        for (unsigned cs = 0; cs < 2; ++cs) {
            nor_transfer(host, cs ? b : a, 0x9f, 0, false, NULL, id, 3, false, NULL);
            payload(host, cs ? "jedec_cs1_mode3" : "jedec_cs0_mode0", id, 3);
            check(host, "jedec_id", id[0] == 0xef && id[1] == 0x40 && id[2] == 0x14);
        }
        check(host, "initial_status", nor_status(host, a) == 0);
        for (unsigned i = 0; i < sizeof(wanted); ++i) wanted[i] = (uint8_t)(i * 37 + host * 11);
        nor_transfer(host, a, 0x02, 0x100, true, wanted, NULL, sizeof(wanted), false, NULL);
        nor_transfer(host, a, 0x03, 0x100, true, NULL, got, sizeof(got), false, NULL);
        payload(host, "no_wren48", got, sizeof(got));
        check(host, "program_without_wren_ignored", erased(got, sizeof(got)));
        nor_wren(host, a);
        check(host, "two_cs_wel_isolation", nor_status(host, b) == 0);
        nor_transfer(host, a, 0x02, 0x100, true, wanted, NULL, sizeof(wanted), false, NULL);
        bool program_busy = (nor_status(host, a) & 3) == 1;
        nor_wait(host, a, "program_ready");
        check(host, "program_busy", program_busy);
        nor_transfer(host, a, 0x03, 0x100, true, NULL, got, sizeof(got), false, NULL);
        payload(host, "program_read48", got, sizeof(got));
        check(host, "program_read_exact", memcmp(wanted, got, sizeof(got)) == 0);
        nor_transfer(host, b, 0x03, 0x100, true, NULL, got, sizeof(got), false, NULL);
        payload(host, "other_cs48", got, sizeof(got));
        check(host, "two_cs_storage_isolation", erased(got, sizeof(got)));
        memset(wanted, 0xff, sizeof(wanted));
        nor_wren(host, a);
        nor_transfer(host, a, 0x02, 0x100, true, wanted, NULL, sizeof(wanted), false, NULL);
        nor_wait(host, a, "and_program_ready");
        nor_transfer(host, a, 0x03, 0x100, true, NULL, got, sizeof(got), false, NULL);
        payload(host, "one_to_zero48", got, sizeof(got));
        for (unsigned i = 0; i < sizeof(wanted); ++i) wanted[i] = (uint8_t)(i * 37 + host * 11);
        check(host, "nor_one_to_zero_only", memcmp(wanted, got, sizeof(got)) == 0);
        nor_wren(host, a);
        nor_transfer(host, a, 0x20, 0, true, NULL, NULL, 0, false, NULL);
        bool erase_busy = (nor_status(host, a) & 3) == 1;
        nor_wait(host, a, "erase_ready");
        check(host, "erase_busy", erase_busy);
        nor_transfer(host, a, 0x03, 0x100, true, NULL, got, sizeof(got), false, NULL);
        payload(host, "erased48", got, sizeof(got));
        check(host, "erase_exact", erased(got, sizeof(got)));
    }
    if (a) status(host, "remove_device", spi_bus_remove_device(a));
    if (b) status(host, "remove_device", spi_bus_remove_device(b));
    status(host, "bus_free", spi_bus_free((spi_host_device_t)(host - 1)));
}
static void nor_dma(unsigned host)
{
    if (!bus_init(host, true)) return;
    spi_device_handle_t a = add_device(host, 0, 0, true, false, true);
    spi_device_handle_t b = add_device(host, 1, 3, true, false, true);
    uint8_t *wanted = spi_bus_dma_memory_alloc((spi_host_device_t)(host - 1), LARGE_BYTES, 0);
    uint8_t *got = spi_bus_dma_memory_alloc((spi_host_device_t)(host - 1), LARGE_BYTES, 0);
    check(host, "dma_alloc", wanted && got);
    if (a && b && wanted && got) {
        for (unsigned i = 0; i < LARGE_BYTES; ++i) wanted[i] = (uint8_t)((i * 73) ^ (i >> 7) ^ (host * 31));
        for (unsigned offset = 0; offset < LARGE_BYTES; offset += 256) {
            size_t size = LARGE_BYTES - offset < 256 ? LARGE_BYTES - offset : 256;
            nor_wren(host, a);
            nor_transfer(host, a, 0x02, NOR_BASE + offset, true, wanted + offset, NULL, size, true, NULL);
            nor_wait(host, a, "dma_page_ready");
        }
        event e = {.host = host};
        memset(got, 0xa5, LARGE_BYTES);
        if (nor_transfer(host, a, 0x03, NOR_BASE, true, NULL, got, LARGE_BYTES, true, &e)) {
            payload(host, "nor_dma8201", got, LARGE_BYTES);
            check(host, "nor_dma_exact", memcmp(wanted, got, LARGE_BYTES) == 0);
            report_event(&e, "nor_dma_descriptors_irq", true, false, LARGE_BYTES);
        }
        e = (event){.host = host};
        if (nor_transfer(host, b, 0x03, NOR_BASE, true, NULL, got, LARGE_BYTES, true, &e)) {
            payload(host, "other_cs_dma8201", got, LARGE_BYTES);
            check(host, "nor_dma_two_cs", erased(got, LARGE_BYTES));
            report_event(&e, "other_cs_dma_descriptors_irq", true, false, LARGE_BYTES);
        }
    }
    free(wanted); free(got);
    if (a) status(host, "remove_device", spi_bus_remove_device(a));
    if (b) status(host, "remove_device", spi_bus_remove_device(b));
    status(host, "bus_free", spi_bus_free((spi_host_device_t)(host - 1)));
}
static void controlled(unsigned host, spi_device_handle_t device)
{
    uintptr_t base = spi_base(host);
    gpio_input_enable(pins[host - 2][3]);
    const char *names[] = {"soft_reset_cancel", "local_gate_cancel", "system_gate_cancel", "system_reset_cancel"};
    for (unsigned which = 0; which < 4; ++which) {
        uint32_t tx = 0x6935c7a1, rx = 0;
        spi_transaction_t warmup = {.length = 32, .tx_buffer = &tx, .rx_buffer = &rx};
        if (!status(host, "controlled_warmup", spi_device_polling_transmit(device, &warmup))) return;
        check(host, "controlled_warmup_exact", tx == rx);
        /* IDF6.1 spi_device_acquire_bus only supports portMAX_DELAY (finite
         * wait is ESP_ERR_INVALID_ARG by API design; first-execution repair). */
        if (!status(host, "controlled_acquire", spi_device_acquire_bus(device, portMAX_DELAY))) return;
        uint32_t saved[13];
        for (unsigned i = 0; i < 13; ++i) saved[i] = REG_READ(base + 4 + i * 4);
        uint32_t slave = REG_READ(base + 0xe0), gate = REG_READ(base + 0xe8);
        uint32_t sysclk = REG_READ(SYSTEM_PERIP_CLK_EN0_REG), sysrst = REG_READ(SYSTEM_PERIP_RST_EN0_REG);
        uint32_t sysbit = host == 2 ? SYSTEM_SPI2_CLK_EN : SYSTEM_SPI3_CLK_EN;
        REG_WRITE(base + 0x34, 0); /* Raw transfer does not enter the driver's ISR. */
        REG_WRITE(base + 0x38, 0xffffffff);
        /* Maximum legal S3 divider: pre=15, N=63, H=floor((63+1)/2-1)=31,
         * L=N (TRM spi_struct.h clock fields are 6/6/6/4 bits; the original
         * 79/99/49/99 poke overflowed every field and truncated to an
         * invalid H+1>N+1 duty). 80MHz/(16*64)=78.125kHz: 512 bits is a
         * 6.5536ms uncancelled transfer, cancelled at +50us below. */
        REG_WRITE(base + 0x0c, (15u << 18) | (63u << 12) | (31u << 6) | 63u);
        REG_WRITE(base + 0x1c, 511);
        for (unsigned i = 0; i < 16; ++i) REG_WRITE(base + 0x98 + 4 * i, 0x93a6c571 ^ i);
        REG_WRITE(base, SPI_UPDATE);
        int64_t start = esp_timer_get_time();
        REG_WRITE(base, SPI_USR);
        esp_rom_delay_us(50);
        bool active = (REG_READ(base) & SPI_USR) != 0;
        if (which == 0) REG_WRITE(base + 0xe0, slave | BIT(27));
        if (which == 1) REG_WRITE(base + 0xe8, 0);
        if (which == 2) REG_WRITE(SYSTEM_PERIP_CLK_EN0_REG, sysclk & ~sysbit);
        if (which == 3) REG_WRITE(SYSTEM_PERIP_RST_EN0_REG, sysrst | sysbit);
        esp_rom_delay_us(60000); /* Past the uncancelled 6.5536ms transfer deadline. */
        uint32_t command = REG_READ(base), raw = REG_READ(base + 0x3c);
        printf("SPI_NATIVE_CONTROL host=%u name=%s was_active=%u command=%08" PRIx32 " raw=%08" PRIx32 " elapsed_us=%" PRId64 "\n", host, names[which], active, command, raw, esp_timer_get_time() - start);
        /* Aborted outputs are released, not an implicit high rail. Reading
         * an unpulled CS through GPIO_IN would consume an unknown net. */
        check(host, names[which], active && !(command & SPI_USR) && !(raw & SPI_TRANS_DONE_INT_RAW));
        REG_WRITE(SYSTEM_PERIP_CLK_EN0_REG, sysclk);
        REG_WRITE(SYSTEM_PERIP_RST_EN0_REG, sysrst);
        REG_WRITE(base + 0xe0, slave);
        REG_WRITE(base + 0xe8, gate);
        for (unsigned i = 0; i < 13; ++i) REG_WRITE(base + 4 + i * 4, saved[i]);
        REG_WRITE(base + 0x38, 0xffffffff);
        spi_device_release_bus(device);
        rx = 0;
        status(host, "controlled_recovery", spi_device_polling_transmit(device, &warmup));
        check(host, "controlled_recovery_exact", rx == tx);
    }
}
static uint8_t reverse8(uint8_t value)
{
    value = ((value & 0x55) << 1) | ((value >> 1) & 0x55);
    value = ((value & 0x33) << 2) | ((value >> 2) & 0x33);
    return (value << 4) | (value >> 4);
}
static void loopback_pio(unsigned host)
{
    if (!bus_init(host, false)) return;
    const char *orders[] = {"msb", "lsb", "txlsb_rxmsb", "txmsb_rxlsb"};
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned order = 0; order < 4; ++order) {
        spi_device_handle_t device = add_device(host, 0, mode, false, order, false);
        if (!device) continue;
        uint8_t tx[61], rx[61] = {0}, expected[61];
        for (unsigned i = 0; i < sizeof(tx); ++i) {
            tx[i] = (uint8_t)(i * 29 + mode * 17 + order * 71 + host);
            expected[i] = order >= 2 ? reverse8(tx[i]) : tx[i];
        }
        spi_transaction_t t = {.length = sizeof(tx) * 8, .tx_buffer = tx, .rx_buffer = rx};
        status(host, "loopback_polling", spi_device_polling_transmit(device, &t));
        char name[48];
        snprintf(name, sizeof(name), "loopback_mode%u_%s61", mode, orders[order]);
        payload(host, name, rx, sizeof(rx));
        check(host, name, memcmp(expected, rx, sizeof(rx)) == 0);
        memset(rx, 0, sizeof(rx));
        spi_transaction_ext_t bits = {.base = {.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY,
            .cmd = 0x15, .addr = 0x56b, .length = 13, .rxlength = 13, .tx_buffer = tx, .rx_buffer = rx},
            .command_bits = 5, .address_bits = 11, .dummy_bits = 3};
        int64_t before = esp_timer_get_time();
        status(host, "nonbyte_phases_polling", spi_device_polling_transmit(device, &bits.base));
        snprintf(name, sizeof(name), "nonbyte_mode%u_%s13", mode, orders[order]);
        payload(host, name, rx, 2);
        uint8_t mask = order == 1 || order == 3 ? 0x1f : 0xf8;
        check(host, name, rx[0] == expected[0] && (rx[1] & mask) == (expected[1] & mask));
        printf("SPI_NATIVE_BITS host=%u mode=%u order=%s command_bits=5 address_bits=11 dummy_bits=3 data_bits=13 elapsed_us=%" PRId64 " user=%08" PRIx32 " user1=%08" PRIx32 " user2=%08" PRIx32 " dlen=%08" PRIx32 "\n",
               host, mode, orders[order], esp_timer_get_time() - before,
               REG_READ(spi_base(host) + 0x10), REG_READ(spi_base(host) + 0x14),
               REG_READ(spi_base(host) + 0x18), REG_READ(spi_base(host) + 0x1c));
        if (mode == 0 && !order) controlled(host, device);
        if (mode == 0 && !order) {
            uint32_t saved_cpu = REG_READ(SYSTEM_SYSCLK_CONF_REG);
            memset(rx, 0, sizeof(rx));
            before = esp_timer_get_time();
            /* IDF6.1 spi_device_polling_start only supports portMAX_DELAY. */
            if (status(host, "pll_xtal_start", spi_device_polling_start(device, &t, portMAX_DELAY))) {
                REG_SET_FIELD(SYSTEM_SYSCLK_CONF_REG, SYSTEM_SOC_CLK_SEL, 0);
                unsigned cpu_mux = REG_GET_FIELD(SYSTEM_SYSCLK_CONF_REG, SYSTEM_SOC_CLK_SEL);
                bool pll_source = REG_READ(spi_base(host) + 0xe8) & BIT(2);
                bool pll_powered = !(REG_READ(0x60008000) & BIT(10));
                status(host, "pll_xtal_end", spi_device_polling_end(device, WAIT_TICKS));
                int64_t elapsed = esp_timer_get_time() - before;
                REG_WRITE(SYSTEM_SYSCLK_CONF_REG, saved_cpu);
                payload(host, "pll_cpu_xtal61", rx, sizeof(rx));
                check(host, "pll_cpu_xtal_exact", !cpu_mux && pll_source && pll_powered &&
                      memcmp(tx, rx, sizeof(rx)) == 0);
                printf("SPI_NATIVE_PLL host=%u cpu_mux=%u pll_source=%u pll_powered=%u elapsed_us=%" PRId64 "\n",
                       host, cpu_mux, pll_source, pll_powered, elapsed);
            }
        }
        status(host, "remove_device", spi_bus_remove_device(device));
    }
    status(host, "bus_free", spi_bus_free((spi_host_device_t)(host - 1)));
}
static void loopback_dma(unsigned host)
{
    if (!bus_init(host, true)) return;
    spi_device_handle_t device = add_device(host, 0, 0, false, false, true);
    uint8_t *tx = spi_bus_dma_memory_alloc((spi_host_device_t)(host - 1), STORAGE_BYTES * 2, 0);
    uint8_t *rx = spi_bus_dma_memory_alloc((spi_host_device_t)(host - 1), STORAGE_BYTES * 2, 0);
    check(host, "dma_alloc", tx && rx);
    if (device && tx && rx) {
        spi_transaction_t t[2];
        event e[2] = {{.host = host}, {.host = host}};
        memset(t, 0, sizeof(t));
        memset(rx, 0xa5, STORAGE_BYTES * 2);
        unsigned queued = 0;
        for (unsigned slot = 0; slot < 2; ++slot) {
            for (unsigned i = 0; i < LARGE_BYTES; ++i) tx[slot * STORAGE_BYTES + i] = (uint8_t)((i * 73) ^ (i >> 7) ^ (host * 31) ^ (slot * 0xa7));
            t[slot] = (spi_transaction_t){.length = LARGE_BYTES * 8, .tx_buffer = tx + slot * STORAGE_BYTES,
                .rx_buffer = rx + slot * STORAGE_BYTES, .user = &e[slot]};
            if (!status(host, "loopback_queue", spi_device_queue_trans(device, &t[slot], WAIT_TICKS))) break;
            queued++;
        }
        for (unsigned slot = 0; slot < queued; ++slot) {
            spi_transaction_t *done = NULL;
            if (!status(host, "loopback_result", spi_device_get_trans_result(device, &done, WAIT_TICKS))) break;
            check(host, "queue_order_identity", done == &t[slot]);
            char name[40];
            snprintf(name, sizeof(name), "loopback_dma8201_slot%u", slot);
            payload(host, name, rx + slot * STORAGE_BYTES, LARGE_BYTES);
            check(host, name, memcmp(tx + slot * STORAGE_BYTES, rx + slot * STORAGE_BYTES, LARGE_BYTES) == 0);
            /* Descriptor words are captured inside post_cb, before reuse by the
             * next queued transfer. IRQ/eof values are captured there too. */
            report_event(&e[slot], slot ? "queued1_descriptors_irq" : "queued0_descriptors_irq", true, true, LARGE_BYTES);
        }
        check(host, "two_queued_transactions", queued == 2);
        memset(rx, 0xa5, STORAGE_BYTES);
        event prefix_event = {.host = host};
        spi_transaction_t prefix = {.length = 64, .rxlength = 32,
            .tx_buffer = tx, .rx_buffer = rx, .user = &prefix_event};
        if (status(host, "prefix_queue", spi_device_queue_trans(device, &prefix, WAIT_TICKS))) {
            spi_transaction_t *done = NULL;
            if (status(host, "prefix_result", spi_device_get_trans_result(device, &done, WAIT_TICKS))) {
                payload(host, "full_duplex_rx_prefix4", rx, 4);
                check(host, "full_duplex_rx_prefix_exact", done == &prefix && memcmp(tx, rx, 4) == 0);
                bool untouched = true;
                for (unsigned i = 4; i < STORAGE_BYTES; i++) untouched &= rx[i] == 0xa5;
                check(host, "full_duplex_rx_prefix_bounds", untouched);
                report_event(&prefix_event, "full_duplex_rx_prefix_irq", true, false, 4);
            }
        }
    }
    free(tx); free(rx);
    if (device) status(host, "remove_device", spi_bus_remove_device(device));
    status(host, "bus_free", spi_bus_free((spi_host_device_t)(host - 1)));
}
/* Ordinary APIs cannot represent a preloaded-but-empty next RX descriptor,
 * zero-byte RX EOF, or invalid owner. Reserve channel0 only after every
 * ordinary DMA bus has been freed; ordinary PIO sets the real GPIO routing. */
static void direct_rx_end(bool owner_error)
{
    const unsigned host = 3;
    const uintptr_t base = 0x60025000;
    if (!bus_init(host, false)) return;
    spi_device_handle_t device = add_device(host, 0, 0, false, false, false);
    descriptor *d = heap_caps_calloc(2, sizeof(*d), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *buffer = heap_caps_malloc(8, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    check(host, "direct_dma_alloc", d && buffer);
    if (device && d && buffer) {
        uint32_t tx = 0xc7359a61, rx = 0;
        spi_transaction_t warmup = {.length = 32, .tx_buffer = &tx, .rx_buffer = &rx};
        if (status(host, "direct_route_warmup", spi_device_polling_transmit(device, &warmup)) &&
        /* IDF6.1 spi_device_acquire_bus only supports portMAX_DELAY. */
            status(host, "direct_acquire", spi_device_acquire_bus(device, portMAX_DELAY))) {
            uint32_t saved[13];
            for (unsigned i = 0; i < 13; ++i) saved[i] = REG_READ(base + 4 + i * 4);
            uint32_t sysclk = REG_READ(SYSTEM_PERIP_CLK_EN1_REG);
            uint32_t sysrst = REG_READ(SYSTEM_PERIP_RST_EN1_REG);
            REG_WRITE(SYSTEM_PERIP_CLK_EN1_REG, sysclk | SYSTEM_DMA_CLK_EN);
            REG_WRITE(SYSTEM_PERIP_RST_EN1_REG, sysrst & ~SYSTEM_DMA_RST);
            uint32_t misc = REG_READ(GDMA_MISC_CONF_REG);
            REG_WRITE(GDMA_MISC_CONF_REG, misc | GDMA_CLK_EN);
            const char *names[] = {"direct_full_then_empty", "direct_partial3", "direct_zero_bytes",
                                   "direct_reset_empty", "direct_owner_error"};
            for (unsigned which = owner_error ? 4 : 0; which < (owner_error ? 5 : 4); ++which) {
                memset(buffer, 0xa5, 8);
                d[0] = (descriptor){.control = 4 | (which == 4 ? 0 : BIT(31)),
                                    .buffer = (uintptr_t)buffer, .next = (uintptr_t)&d[1]};
                d[1] = (descriptor){.control = 4 | BIT(31), .buffer = (uintptr_t)(buffer + 4)};
                descriptor untouched = d[1];
                uint32_t before = d[0].control;
                if (owner_error) {
                    printf("SPI_NATIVE_NEGATIVE_ARM host=3 profile=owner_error virtual_us=%" PRId64 " expected=strict_pause_owner_error\n", esp_timer_get_time());
                    printf("SPI_NATIVE_OWNER_ARM d0=%08" PRIxPTR " d1=%08" PRIxPTR " buffer=%08" PRIxPTR "\n",
                           (uintptr_t)&d[0], (uintptr_t)&d[1], (uintptr_t)buffer);
                    fflush(stdout);
                }
                REG_WRITE(GDMA_IN_CONF0_CH0_REG, GDMA_IN_RST_CH0);
                REG_WRITE(GDMA_IN_CONF0_CH0_REG, 0);
                REG_WRITE(GDMA_IN_CONF1_CH0_REG, GDMA_IN_CHECK_OWNER_CH0 | 12);
                REG_WRITE(GDMA_IN_INT_ENA_CH0_REG, 0);
                REG_WRITE(GDMA_IN_INT_CLR_CH0_REG, 0xffffffff);
                REG_WRITE(GDMA_IN_PERI_SEL_CH0_REG, 1); /* SPI3, actual RX peripheral. */
                REG_WRITE(GDMA_IN_LINK_CH0_REG, ((uintptr_t)d & 0xfffff) | GDMA_INLINK_START_CH0);
                for (unsigned i = 0; i < 13; ++i) REG_WRITE(base + 4 + i * 4, saved[i]);
                REG_WRITE(base + 0x34, 0);
                REG_WRITE(base + 0x38, 0xffffffff);
                REG_WRITE(base + 0x30, SPI_DMA_RX_ENA | SPI_RX_AFIFO_RST | SPI_BUF_AFIFO_RST);
                REG_WRITE(base + 0x30, SPI_DMA_RX_ENA);
                REG_WRITE(base + 0x98, tx);
                unsigned bits = which == 1 ? 24 : 32;
                REG_WRITE(base + 0x1c, bits - 1);
                if (which == 2) REG_WRITE(base + 0x10, saved[3] & ~(SPI_USR_MOSI | SPI_USR_MISO));
                if (which == 3) REG_WRITE(base + 0x0c, (79u << 18) | (99u << 12) | (49u << 6) | 99u);
                REG_WRITE(base, SPI_UPDATE);
                int64_t start = esp_timer_get_time();
                REG_WRITE(base, SPI_USR);
                if (which == 3) {
                    esp_rom_delay_us(50); /* Less than one slow SCLK bit, no RX byte. */
                    REG_WRITE(base + 0xe0, REG_READ(base + 0xe0) | BIT(27));
                    esp_rom_delay_us(5000);
                } else {
                    while ((REG_READ(base) & SPI_USR) && esp_timer_get_time() - start < 5000) {}
                }
                uint32_t raw = REG_READ(GDMA_IN_INT_RAW_CH0_REG);
                uint32_t eof = REG_READ(GDMA_IN_SUC_EOF_DES_ADDR_CH0_REG);
                uint32_t spi_raw = REG_READ(base + 0x3c);
                bool untouched_next = memcmp(&d[1], &untouched, sizeof(untouched)) == 0;
                printf("SPI_NATIVE_DIRECT host=3 name=%s d0_address=%08" PRIxPTR " before=%08" PRIx32
                       " after=%08" PRIx32 " d1_before=%08" PRIx32 " d1_after=%08" PRIx32
                       " next_unchanged=%u rx_raw=%08" PRIx32 " eof=%08" PRIx32 " spi_raw=%08" PRIx32
                       " elapsed_us=%" PRId64 "\n", names[which], (uintptr_t)d, before, d[0].control,
                       untouched.control, d[1].control, untouched_next, raw, eof, spi_raw, esp_timer_get_time() - start);
                payload(host, names[which], buffer, 8);
                bool good = untouched_next;
                if (which < 2) {
                    size_t size = which == 0 ? 4 : 3;
                    good &= d[0].control == (4 | (size << 12) | BIT(30)) &&
                            (raw & GDMA_IN_SUC_EOF_CH0_INT_RAW) && eof == (uintptr_t)d &&
                            (spi_raw & SPI_TRANS_DONE_INT_RAW) && !(REG_READ(base) & SPI_USR) &&
                            memcmp(buffer, &tx, size) == 0;
                    for (unsigned i = size; i < 8; ++i) good &= buffer[i] == 0xa5;
                } else {
                    good &= d[0].control == before && !(raw & GDMA_IN_SUC_EOF_CH0_INT_RAW);
                    for (unsigned i = 0; i < 8; ++i) good &= buffer[i] == 0xa5;
                    if (which == 3) good &= !(spi_raw & SPI_TRANS_DONE_INT_RAW);
                    if (which == 4) good &= !!(raw & GDMA_IN_DSCR_ERR_CH0_INT_RAW);
                }
                check(host, names[which], good);
                REG_WRITE(GDMA_IN_INT_CLR_CH0_REG, 0xffffffff);
                uint32_t finished = d[0].control;
                esp_rom_delay_us(100);
                check(host, "direct_no_duplicate_eof", d[0].control == finished &&
                      !(REG_READ(GDMA_IN_INT_RAW_CH0_REG) & GDMA_IN_SUC_EOF_CH0_INT_RAW));
                REG_WRITE(base + 0xe0, REG_READ(base + 0xe0) | BIT(27));
                REG_WRITE(base + 0xe0, 0);
                REG_WRITE(base + 0xe8, 7);
            }
            REG_WRITE(GDMA_IN_LINK_CH0_REG, GDMA_INLINK_STOP_CH0);
            REG_WRITE(GDMA_IN_CONF0_CH0_REG, GDMA_IN_RST_CH0);
            REG_WRITE(GDMA_IN_CONF0_CH0_REG, 0);
            REG_WRITE(GDMA_IN_PERI_SEL_CH0_REG, 63);
            REG_WRITE(GDMA_MISC_CONF_REG, misc);
            REG_WRITE(SYSTEM_PERIP_CLK_EN1_REG, sysclk);
            REG_WRITE(SYSTEM_PERIP_RST_EN1_REG, sysrst);
            for (unsigned i = 0; i < 13; ++i) REG_WRITE(base + 4 + i * 4, saved[i]);
            REG_WRITE(base + 0x38, 0xffffffff);
            spi_device_release_bus(device);
        }
    }
    free(d); free(buffer);
    if (device) status(host, "remove_device", spi_bus_remove_device(device));
    status(host, "bus_free", spi_bus_free(SPI3_HOST));
}
static const char *profile(void)
{
#if CONFIG_SPI_NATIVE_PROFILE_LOOPBACK
    return "loopback";
#elif CONFIG_SPI_NATIVE_PROFILE_WRONG_CS
    return "wrong_cs";
#elif CONFIG_SPI_NATIVE_PROFILE_DISCONNECTED_MISO
    return "disconnected_miso";
#elif CONFIG_SPI_NATIVE_PROFILE_OWNER_ERROR
    return "owner_error";
#else
    return "connected";
#endif
}
void app_main(void)
{
    printf("SPI_NATIVE_BOOT profile=%s\n", profile());
    printf("SPI_NATIVE_GATE advanced=UNSUPPORTED slave=UNSUPPORTED psram_dma=UNSUPPORTED spi01=UNTOUCHED\n");
    fflush(stdout);
#if CONFIG_SPI_NATIVE_PROFILE_OWNER_ERROR
    direct_rx_end(true);
    check(3, "owner_error_must_not_complete", false);
#elif CONFIG_SPI_NATIVE_PROFILE_WRONG_CS || CONFIG_SPI_NATIVE_PROFILE_DISCONNECTED_MISO
    unsigned host = CONFIG_SPI_NATIVE_NEGATIVE_HOST;
    if (bus_init(host, false)) {
        spi_device_handle_t device = add_device(host, 0, 0, true, false, false);
        if (device) {
            uint8_t id[3] = {0xa5, 0xa5, 0xa5};
            printf("SPI_NATIVE_NEGATIVE_ARM host=%u profile=%s virtual_us=%" PRId64 " expected=strict_pause_unknown_miso\n", host, profile(), esp_timer_get_time());
            fflush(stdout);
            nor_transfer(host, device, 0x9f, 0, false, NULL, id, 3, false, NULL);
            payload(host, "unexpected_negative_rx", id, sizeof(id));
            check(host, "negative_must_not_complete", false);
        }
    }
#else
    for (unsigned host = 2; host <= 3; ++host) {
#if CONFIG_SPI_NATIVE_PROFILE_LOOPBACK
        loopback_pio(host);
        loopback_dma(host);
#else
        nor_pio(host);
        nor_dma(host);
#endif
    }
#if CONFIG_SPI_NATIVE_PROFILE_LOOPBACK
    direct_rx_end(false);
#endif
#endif
    printf("SPI_NATIVE_DONE profile=%s failures=%u result=%s virtual_us=%" PRId64 "\n", profile(), failures, failures ? "FAIL" : "PASS", esp_timer_get_time());
    fflush(stdout);
}
