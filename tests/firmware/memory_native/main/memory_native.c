#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_memory_utils.h"
#include "esp_mmu_map.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_private/gdma.h"
#include "hal/dma_types.h"
#include "hal/gdma_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_SPIRAM_MODE_QUAD
#include "esp_private/cache_utils.h"
#include "hal/psram_ctrlr_ll.h"
#include "soc/spi_mem_struct.h"
#endif
#include "soc/ext_mem_defs.h"
#include "soc/soc.h"

/* Descriptor memory is always internal. No async memcpy driver or CPU memcpy
 * substitutes for a GDMA transaction, including the deliberately invalid ones. */
enum { NODES = 3, NODE_BYTES = 128, BYTES = NODES * NODE_BYTES };
static unsigned failures;
static gdma_channel_handle_t tx, rx;
static int tx_id, rx_id;
static dma_descriptor_t *out_desc, *in_desc;
static const char *profile;

static bool check(const char *name, bool ok)
{
    printf("MEMORY_NATIVE_CHECK name=%s result=%s\n", name, ok ? "PASS" : "FAIL");
    failures += !ok;
    return ok;
}

static bool api(const char *name, esp_err_t err)
{
    printf("MEMORY_NATIVE_API name=%s code=%d\n", name, err);
    return check(name, err == ESP_OK);
}

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; ++i) p[i] = (i * 37 + seed) & 255;
    __asm__ volatile("memw" ::: "memory");
}

static bool matches(const volatile uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != ((i * 37 + seed) & 255)) return false;
    return true;
}

static void observation(const char *name, const uint8_t *p, size_t n)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < n; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    printf("MEMORY_NATIVE_BYTES name=%s length=%u fnv32=%08" PRIx32 " prefix=", name, (unsigned)n, hash);
    for (size_t i = 0; i < 16 && i < n; ++i) printf("%02x", p[i]);
    printf("\n");
}

/* Only these private data partitions are writable. Phase and bytes must come
 * from flash, never RTC/noinit state. Each boot verifies the last committed
 * phase before replacing it; the host controls reset and process relaunch. */
enum { PERSIST_BYTES = 769, RAW_OFFSET = 37 };

static void persistent_pattern(uint8_t *p, uint32_t phase)
{
    for (unsigned i = 0; i < PERSIST_BYTES; ++i)
        p[i] = (uint8_t)((i * 73 + (i >> 3) * 29 + phase * 53) ^ (0xa7 + i * 11));
}

static void partition_evidence(const char *name, const esp_partition_t *part)
{
    printf("MEMORY_NATIVE_PARTITION name=%s label=%s type=%u subtype=%u address=%08" PRIx32
           " size=%08" PRIx32 " encrypted=%u\n", name, part->label, part->type,
           part->subtype, part->address, part->size, part->encrypted);
}

static bool raw_readback(const esp_partition_t *raw, const char *name, const uint8_t *expected)
{
    uint8_t data[PERSIST_BYTES], direct[PERSIST_BYTES];
    if (!api("raw_partition_read", esp_partition_read(raw, RAW_OFFSET, data, sizeof(data))) ||
        !api("raw_flash_read", esp_flash_read(raw->flash_chip, direct, raw->address + RAW_OFFSET, sizeof(direct))))
        return false;
    observation(name, data, sizeof(data));
    return check(name, memcmp(data, expected, sizeof(data)) == 0 &&
                       memcmp(direct, expected, sizeof(direct)) == 0);
}

static bool nvs_readback(nvs_handle_t handle, const char *name, uint32_t phase, const uint8_t *expected)
{
    uint32_t stored = 0;
    size_t length = 0;
    uint8_t data[PERSIST_BYTES];
    if (!api("nvs_get_phase", nvs_get_u32(handle, "phase", &stored)) ||
        !api("nvs_blob_size", nvs_get_blob(handle, "blob", NULL, &length)) ||
        !check("nvs_blob_length", length == sizeof(data))) return false;
    if (!api("nvs_get_blob", nvs_get_blob(handle, "blob", data, &length))) return false;
    observation(name, data, length);
    return check(name, stored == phase && length == sizeof(data) &&
                       memcmp(data, expected, sizeof(data)) == 0);
}

static void flash_persistence_cases(void)
{
    const esp_partition_t *app = esp_ota_get_running_partition();
    const esp_partition_t *nvs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    const esp_partition_t *raw = esp_partition_find_first((esp_partition_type_t)0x40,
                                                        (esp_partition_subtype_t)0, "mem_raw");
    if (!check("flash_partitions_found", app && nvs && raw)) return;
    partition_evidence("app", app);
    partition_evidence("nvs", nvs);
    partition_evidence("raw", raw);
    if (!check("flash_partitions_safe", !app->encrypted && !nvs->encrypted && !raw->encrypted &&
               raw->size >= 4096 && raw->erase_size == 4096 && !raw->readonly &&
               app->flash_chip == raw->flash_chip && nvs->flash_chip == raw->flash_chip)) return;
    uint8_t header[32], app_header[32], direct[32];
    uint32_t flash_size = 0;
    if (!api("flash_size", esp_flash_get_size(NULL, &flash_size)) ||
        !check("flash_size_4m", flash_size == 4 * 1024 * 1024) ||
        !api("flash_first_header", esp_flash_read(NULL, header, 0, sizeof(header))) ||
        !api("app_partition_header", esp_partition_read(app, 0, app_header, sizeof(app_header))) ||
        !api("app_flash_header", esp_flash_read(app->flash_chip, direct, app->address, sizeof(direct)))) return;
    observation("flash_first_header", header, sizeof(header));
    observation("app_first_header", app_header, sizeof(app_header));
    if (!check("flash_headers", header[0] == 0xe9 && app_header[0] == 0xe9 &&
               memcmp(app_header, direct, sizeof(direct)) == 0)) return;
    /* No erase-on-init-error fallback: it would hide loss of persisted data. */
    if (!api("nvs_init", nvs_flash_init_partition(nvs->label))) return;
    nvs_handle_t handle;
    if (!api("nvs_open", nvs_open_from_partition(nvs->label, "memory_fixture", NVS_READWRITE, &handle))) return;
    uint32_t phase = 0;
    esp_err_t err = nvs_get_u32(handle, "phase", &phase);
    uint8_t expected[PERSIST_BYTES], blank[PERSIST_BYTES];
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        size_t length = 0;
        if (!check("nvs_fresh", nvs_get_blob(handle, "blob", NULL, &length) == ESP_ERR_NVS_NOT_FOUND)) goto close;
        memset(blank, 0xff, sizeof(blank));
        if (!raw_readback(raw, "raw_fresh", blank)) goto close;
    } else {
        if (!api("nvs_previous_phase", err) || !check("nvs_phase_range", phase >= 1 && phase <= 2)) goto close;
        persistent_pattern(expected, phase);
        if (!nvs_readback(handle, "nvs_previous", phase, expected) ||
            !raw_readback(raw, "raw_previous", expected)) goto close;
    }
    printf("MEMORY_NATIVE_PERSIST previous=%" PRIu32 " next=%" PRIu32 " length=%u raw_offset=%u\n",
           phase, phase + 1, PERSIST_BYTES, RAW_OFFSET);
    persistent_pattern(expected, phase + 1);
    if (!api("raw_erase", esp_partition_erase_range(raw, 0, 4096)) ||
        !api("raw_write", esp_partition_write(raw, RAW_OFFSET, expected, sizeof(expected))) ||
        !raw_readback(raw, "raw_committed", expected) ||
        !api("nvs_set_blob", nvs_set_blob(handle, "blob", expected, sizeof(expected))) ||
        !api("nvs_set_phase", nvs_set_u32(handle, "phase", phase + 1)) ||
        !api("nvs_commit", nvs_commit(handle))) goto close;
    nvs_close(handle);
    if (!api("nvs_reopen", nvs_open_from_partition(nvs->label, "memory_fixture", NVS_READONLY, &handle))) return;
    nvs_readback(handle, "nvs_committed", phase + 1, expected);
close:
    nvs_close(handle);
}

static void descriptors(uint8_t *src, uint8_t *dst)
{
    memset(out_desc, 0, NODES * sizeof(*out_desc));
    memset(in_desc, 0, NODES * sizeof(*in_desc));
    for (unsigned i = 0; i < NODES; ++i) {
        out_desc[i].dw0.size = out_desc[i].dw0.length = NODE_BYTES;
        out_desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        out_desc[i].dw0.suc_eof = i == NODES - 1;
        out_desc[i].buffer = src + i * NODE_BYTES;
        out_desc[i].next = i + 1 < NODES ? &out_desc[i + 1] : NULL;
        in_desc[i].dw0.size = NODE_BYTES;
        in_desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        in_desc[i].buffer = dst + i * NODE_BYTES;
        in_desc[i].next = i + 1 < NODES ? &in_desc[i + 1] : NULL;
    }
    __asm__ volatile("memw" ::: "memory");
}

static bool transfer(const char *name, uint8_t *src, uint8_t *dst, int expect_error)
{
    descriptors(src, dst);
    if (!api("reset_tx", gdma_reset(tx)) || !api("reset_rx", gdma_reset(rx))) return false;
    gdma_ll_tx_clear_interrupt_status(&GDMA, tx_id, GDMA_LL_TX_EVENT_MASK);
    gdma_ll_rx_clear_interrupt_status(&GDMA, rx_id, GDMA_LL_RX_EVENT_MASK);
    if (!api("start_rx", gdma_start(rx, (intptr_t)in_desc)) ||
        !api("start_tx", gdma_start(tx, (intptr_t)out_desc))) return false;
    uint32_t tx_raw = 0, rx_raw = 0;
    int64_t deadline = esp_timer_get_time() + 200000;
    do {
        tx_raw = gdma_ll_tx_get_interrupt_status(&GDMA, tx_id, true);
        rx_raw = gdma_ll_rx_get_interrupt_status(&GDMA, rx_id, true);
        if ((tx_raw & GDMA_LL_EVENT_TX_DESC_ERROR) || (rx_raw & GDMA_LL_EVENT_RX_DESC_ERROR) ||
            ((rx_raw & GDMA_LL_EVENT_RX_SUC_EOF) && (tx_raw & GDMA_LL_EVENT_TX_TOTAL_EOF))) break;
    } while (esp_timer_get_time() < deadline);
    gdma_stop(tx);
    gdma_stop(rx);
    __asm__ volatile("memw" ::: "memory");
    printf("MEMORY_NATIVE_DMA name=%s tx_raw=%08" PRIx32 " rx_raw=%08" PRIx32 " nodes=%u\n",
           name, tx_raw, rx_raw, NODES);
    bool ok;
    if (expect_error) {
        ok = (expect_error == 1 ? !!(tx_raw & GDMA_LL_EVENT_TX_DESC_ERROR) :
                                !!(rx_raw & GDMA_LL_EVENT_RX_DESC_ERROR)) &&
             !(rx_raw & GDMA_LL_EVENT_RX_SUC_EOF);
    } else {
        ok = (rx_raw & GDMA_LL_EVENT_RX_SUC_EOF) && (tx_raw & GDMA_LL_EVENT_TX_TOTAL_EOF) &&
             !(tx_raw & GDMA_LL_EVENT_TX_DESC_ERROR) && !(rx_raw & GDMA_LL_EVENT_RX_DESC_ERROR);
        for (unsigned i = 0; i < NODES; ++i) {
            ok &= in_desc[i].dw0.length == NODE_BYTES &&
                  in_desc[i].dw0.owner == DMA_DESCRIPTOR_BUFFER_OWNER_CPU &&
                  out_desc[i].dw0.owner == DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
        }
        ok &= in_desc[NODES - 1].dw0.suc_eof;
    }
    return check(name, ok);
}

static bool configure(unsigned burst, bool external)
{
    gdma_transfer_config_t cfg = { .max_data_burst_size = burst, .access_ext_mem = external };
    if (!api("configure_tx", gdma_config_transfer(tx, &cfg)) ||
        !api("configure_rx", gdma_config_transfer(rx, &cfg))) return false;
    size_t int_alignment = 0, ext_alignment = 0;
    if (!api("rx_alignment", gdma_get_alignment_constraints(rx, &int_alignment, &ext_alignment))) return false;
    printf("MEMORY_NATIVE_ALIGNMENT burst=%u external=%u internal_alignment=%u external_alignment=%u\n",
           burst, external, (unsigned)int_alignment, (unsigned)ext_alignment);
    return check("supported_alignment", int_alignment == 4 &&
                 ext_alignment == (external ? burst : UINT32_C(0x80000000)));
}

static bool setup(void)
{
    gdma_channel_alloc_config_t cfg = {0};
#if ESP_IDF_VERSION_MAJOR >= 6
    if (!api("allocate_pair", gdma_new_ahb_channel(&cfg, &tx, &rx))) return false;
#else
    cfg.direction = GDMA_CHANNEL_DIRECTION_TX;
    cfg.flags.reserve_sibling = 1;
    if (!api("allocate_tx", gdma_new_ahb_channel(&cfg, &tx))) return false;
    cfg.direction = GDMA_CHANNEL_DIRECTION_RX;
    cfg.flags.reserve_sibling = 0;
    cfg.sibling_chan = tx;
    if (!api("allocate_rx", gdma_new_ahb_channel(&cfg, &rx))) return false;
#endif
    if (!api("tx_id", gdma_get_channel_id(tx, &tx_id)) || !api("rx_id", gdma_get_channel_id(rx, &rx_id))) return false;
    if (!check("sibling_pair", tx_id == rx_id)) return false;
    uint32_t free_ids = 0;
    if (!api("free_trigger", gdma_get_free_m2m_trig_id_mask(tx, &free_ids)) || !check("trigger_available", free_ids != 0)) return false;
    gdma_trigger_t trigger = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
    trigger.instance_id = __builtin_ctz(free_ids);
    if (!api("connect_tx", gdma_connect(tx, trigger)) || !api("connect_rx", gdma_connect(rx, trigger))) return false;
    gdma_strategy_config_t strategy = { .owner_check = true, .auto_update_desc = true, .eof_till_data_popped = true };
    if (!api("strategy_tx", gdma_apply_strategy(tx, &strategy)) || !api("strategy_rx", gdma_apply_strategy(rx, &strategy))) return false;
    out_desc = heap_caps_aligned_alloc(16, NODES * sizeof(*out_desc), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    in_desc = heap_caps_aligned_alloc(16, NODES * sizeof(*in_desc), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    return check("internal_descriptors", out_desc && in_desc && esp_ptr_internal(out_desc) && esp_ptr_internal(in_desc));
}

#if CONFIG_SPIRAM_MODE_QUAD
/* Same exit-QPI / RDID / enter-QPI sequence as the locked IDF quad driver.
 * Run from IRAM under the SDK flash/cache guard; never access flash or PSRAM
 * while the part is temporarily in SPI command mode. Restore SPI1 registers. */
static uint32_t IRAM_ATTR read_quad_id(void)
{
    uint32_t id = 0;
    spi_flash_disable_interrupts_caches_and_other_cpu();
    uint32_t ctrl = SPIMEM1.ctrl.val, user = SPIMEM1.user.val;
    uint32_t user1 = SPIMEM1.user1.val, user2 = SPIMEM1.user2.val;
    uint32_t misc = SPIMEM1.misc.val, mosi = SPIMEM1.mosi_dlen.val, miso = SPIMEM1.miso_dlen.val;
    uint32_t addr = SPIMEM1.addr;
    esp_rom_spi_set_op_mode(1, ESP_ROM_SPIFLASH_QIO_MODE);
    psram_ctrlr_ll_enable_quad_command(1, true);
    psram_ctrlr_ll_common_transaction_base(1, ESP_ROM_SPIFLASH_QIO_MODE, 0xf5, 8, 0, 0, 0, NULL, 0, NULL, 0, 2, false);
    esp_rom_spi_set_op_mode(1, ESP_ROM_SPIFLASH_SLOWRD_MODE);
    psram_ctrlr_ll_common_transaction_base(1, ESP_ROM_SPIFLASH_SLOWRD_MODE, 0x9f, 8, 0, 24, 0, NULL, 0, (uint8_t *)&id, 24, 2, false);
    psram_ctrlr_ll_common_transaction_base(1, ESP_ROM_SPIFLASH_SLOWRD_MODE, 0x35, 8, 0, 0, 0, NULL, 0, NULL, 0, 2, false);
    SPIMEM1.ctrl.val = ctrl;
    SPIMEM1.user.val = user;
    SPIMEM1.user1.val = user1;
    SPIMEM1.user2.val = user2;
    SPIMEM1.misc.val = misc;
    SPIMEM1.mosi_dlen.val = mosi;
    SPIMEM1.miso_dlen.val = miso;
    SPIMEM1.addr = addr;
    spi_flash_enable_interrupts_caches_and_other_cpu();
    return id;
}
#endif

#if CONFIG_SPIRAM
static bool sync_buffer(void *p, size_t n, bool c2m)
{
    return api(c2m ? "msync_c2m" : "msync_m2c", esp_cache_msync(p, n,
        ESP_CACHE_MSYNC_FLAG_TYPE_DATA | (c2m ? ESP_CACHE_MSYNC_FLAG_DIR_C2M : ESP_CACHE_MSYNC_FLAG_DIR_M2C)));
}

static void psram_cases(uint8_t *internal, uint8_t *scratch)
{
    uint8_t *a = heap_caps_aligned_alloc(64, BYTES + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *b = heap_caps_aligned_alloc(64, BYTES + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!check("external_payloads", a && b && esp_ptr_external_ram(a) && esp_ptr_external_ram(b))) goto release;
    printf("MEMORY_NATIVE_ALLOC src=%p dst=%p out_desc=%p in_desc=%p\n", a, b, out_desc, in_desc);
    if (!configure(32, true)) goto release;
    fill(a, BYTES, 11);
    if (!sync_buffer(a, BYTES, true)) goto release;
    fill(a, BYTES, 71);
    transfer("dirty_c2m_before", a, internal, 0);
    check("dirty_cpu_new", matches(a, BYTES, 71));
    check("dirty_dma_old", matches(internal, BYTES, 11));
    observation("dirty_dma_old", internal, BYTES);
    if (!sync_buffer(a, BYTES, true)) goto release;
    transfer("dirty_c2m_after", a, internal, 0);
    check("writeback_dma_new", matches(internal, BYTES, 71));
    observation("writeback_dma_new", internal, BYTES);

    fill(b, BYTES, 19);
    if (!sync_buffer(b, BYTES, true)) goto release;
    check("prime_rx_cache", matches(b, BYTES, 19));
    fill(internal, BYTES, 97);
    transfer("stale_m2c_before", internal, b, 0);
    check("stale_cpu_old", matches(b, BYTES, 19));
    transfer("physical_rx_new", b, scratch, 0);
    check("physical_dma_new", matches(scratch, BYTES, 97));
    observation("physical_dma_new", scratch, BYTES);
    if (!sync_buffer(b, BYTES, false)) goto release;
    check("invalidate_cpu_new", matches(b, BYTES, 97));
    observation("invalidate_cpu_new", b, BYTES);

    for (unsigned burst = 16; burst <= 64; burst *= 2) {
        if (!configure(burst, true)) break;
        fill(a, BYTES, burst + 3);
        fill(b, BYTES, 0);
        if (!sync_buffer(a, BYTES, true) || !sync_buffer(b, BYTES, true)) break;
        char name[48];
        snprintf(name, sizeof(name), "ext_to_ext_burst%u", burst);
        transfer(name, a, b, 0);
        if (!sync_buffer(b, BYTES, false)) break;
        check(name, matches(b, BYTES, burst + 3));
        observation(name, b, BYTES);
        fill(internal, BYTES, burst + 121);
        snprintf(name, sizeof(name), "int_to_ext_burst%u", burst);
        transfer(name, internal, a, 0);
        if (!sync_buffer(a, BYTES, false)) break;
        check(name, matches(a, BYTES, burst + 121));
        observation(name, a, BYTES);
        snprintf(name, sizeof(name), "ext_to_int_burst%u", burst);
        transfer(name, a, scratch, 0);
        check(name, matches(scratch, BYTES, burst + 121));
        observation(name, scratch, BYTES);
        /* RX payload intentionally violates the selected external block alignment.
         * Never CPU-read that destination afterwards or allow cache writeback to
         * overwrite a partial DMA result. Reinitialise it only after M2C. */
        snprintf(name, sizeof(name), "misaligned_rx_burst%u", burst);
        transfer(name, internal, a + 1, 2);
        sync_buffer(a, BYTES, false);
    }

    /* A heap-owned span crossing a real 64KiB MMU page, without remapping
     * allocator metadata or assuming that adjacent physical pages are linear. */
    uint8_t *span = heap_caps_aligned_alloc(64, 65536 + 2 * BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (check("boundary_allocation", span != NULL)) {
        uintptr_t page = ((uintptr_t)span + 65535) & ~(uintptr_t)65535;
        if (page - (uintptr_t)span < BYTES) page += 65536;
        uint8_t *cross = (uint8_t *)(page - NODE_BYTES);
        fill(cross, BYTES, 153);
        sync_buffer(cross, BYTES, true);
        transfer("mmu_page_crossing", cross, scratch, 0);
        check("mmu_page_crossing_data", matches(scratch, BYTES, 153));
        observation("mmu_page_crossing_data", scratch, BYTES);
        heap_caps_free(span);
    }
    /* Verify the probe page against the actual MMU table instead of assuming
     * one: IDF's image_process keeps the last DROM entry (511) as its transient
     * boot-partition map page, so it must never be used as the probe. */
    uint32_t probe = 0;
    for (int entry = 0; entry < SOC_MMU_ENTRY_NUM - 1; ++entry) {
        if (*(volatile uint32_t *)(DR_REG_MMU_TABLE + entry * 4) & SOC_MMU_INVALID) {
            probe = UINT32_C(0x3c000000) + (uint32_t)entry * UINT32_C(0x10000);
            break;
        }
    }
    esp_paddr_t paddr = 0;
    mmu_target_t target;
    uint8_t *unmapped = (uint8_t *)(uintptr_t)probe;
    esp_err_t err = probe ? esp_mmu_vaddr_to_paddr(unmapped, &paddr, &target)
                          : ESP_FAIL;
    printf("MEMORY_NATIVE_UNMAPPED address=%p entry_invalid=%d translate_code=%d\n",
           unmapped, (int)(probe != 0), err);
    if (check("unmapped_confirmed", err == ESP_ERR_NOT_FOUND)) {
        transfer("unbacked_tx_dscr_err", unmapped, scratch, 1);
        transfer("unbacked_rx_dscr_err", internal, unmapped, 2);
    }
    printf("MEMORY_NATIVE_SCOPE mmu_remap=qtest capacity_end=qtest psram_reset_persistence=qtest cpu_unmapped=not_qualified gdma_unmapped=qualified_if_pass octal=unsupported\n");
release:
    heap_caps_free(a);
    heap_caps_free(b);
}
#endif

void app_main(void)
{
#if CONFIG_SPIRAM_MODE_OCT
    profile = "octal-unsupported";
#elif CONFIG_SPIRAM
    profile = "psram";
#else
    profile = "internal";
#endif
    printf("MEMORY_NATIVE_BOOT profile=%s idf=%s\n", profile, esp_get_idf_version());
#if CONFIG_SPIRAM_MODE_OCT
    printf("MEMORY_NATIVE_DONE profile=%s failures=1 result=UNSUPPORTED\n", profile);
    return;
#elif CONFIG_SPIRAM
    bool initialized = esp_psram_is_initialized();
    size_t capacity = esp_psram_get_size();
    uint32_t id = initialized ? read_quad_id() : 0;
    printf("MEMORY_NATIVE_PSRAM initialized=%u capacity=%u id=%06" PRIx32 " mode=quad\n", initialized, (unsigned)capacity, id);
    if (!check("psram_initialized", initialized) || !check("psram_capacity_8m", capacity == 8 * 1024 * 1024) ||
        !check("psram_real_id", (id & 255) == 0x0d && ((id >> 8) & 255) == 0x5d &&
               (((id >> 21) & 7) == 2 || ((id >> 16) & 255) == 0x26))) goto done;
#else
    printf("MEMORY_NATIVE_SCOPE psram=not_qualified internal_leaf=only\n");
#endif
    flash_persistence_cases();
    if (!setup()) goto done;
    uint8_t *internal = heap_caps_aligned_alloc(64, BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    uint8_t *scratch = heap_caps_aligned_alloc(64, BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (check("internal_payloads", internal && scratch && esp_ptr_internal(internal) && esp_ptr_internal(scratch))) {
        configure(16, false);
        fill(internal, BYTES, 7);
        transfer("internal_m2m", internal, scratch, 0);
        check("internal_m2m_data", matches(scratch, BYTES, 7));
        observation("internal_m2m_data", scratch, BYTES);
#if CONFIG_SPIRAM
        psram_cases(internal, scratch);
#endif
    }
    heap_caps_free(internal);
    heap_caps_free(scratch);
    api("disconnect_tx", gdma_disconnect(tx));
    api("disconnect_rx", gdma_disconnect(rx));
    api("delete_tx", gdma_del_channel(tx));
    api("delete_rx", gdma_del_channel(rx));
    heap_caps_free(out_desc);
    heap_caps_free(in_desc);
done:
    printf("MEMORY_NATIVE_DONE profile=%s failures=%u result=%s\n", profile, failures, failures ? "FAIL" : "PASS");
    fflush(stdout);
}
