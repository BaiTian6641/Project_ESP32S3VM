/*
 * ESP32-S3 external RAM: committed SSI storage, CPU write-back cache, EDMA MMU.
 * GPL-2.0-or-later. Functional model; no electrical, latency or silicon LRU claim.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "hw/misc/esp32s3_cache.h"

static uint8_t *psram_storage(ESP32S3CacheState *s)
{
    return s->psram ? memory_region_get_ram_ptr(&s->psram->data_mr) : NULL;
}

static uint64_t psram_capacity(ESP32S3CacheState *s)
{
    return s->psram ? memory_region_size(&s->psram->data_mr) : 0;
}

static void psram_fault(ESP32S3CacheState *s, uint32_t vaddr,
                        ESP32S3MMUEntry entry, unsigned code)
{
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_MMU_FAULT_CONTENT)] =
        (entry.val & 0xffff) | (code << 16);
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_MMU_FAULT_VADDR)] = vaddr;
}

/* GDMA uses the CPU DBUS address window and MMU, but never its cache data. */
static bool psram_translate(ESP32S3CacheState *s, hwaddr offset, unsigned size,
                            bool write, uint32_t *physical)
{
    ESP32S3MMUEntry entry = { .val = 1 << 14 };
    uint64_t address;

    if (offset >= ESP32S3_EXTMEM_REGION_SIZE ||
        size > ESP32S3_EXTMEM_REGION_SIZE - offset) {
        goto fault;
    }
    entry = s->mmu[offset / ESP32S3_PAGE_SIZE];
    address = (uint64_t)entry.page_number * ESP32S3_PAGE_SIZE +
              offset % ESP32S3_PAGE_SIZE;
    if (entry.invalid || entry.type != ESP32S3_MMU_TYPE_PSRAM ||
        address >= psram_capacity(s) || size > psram_capacity(s) - address ||
        size > ESP32S3_PAGE_SIZE - offset % ESP32S3_PAGE_SIZE) {
        goto fault;
    }
    *physical = address;
    return true;
fault:
    psram_fault(s, ESP32S3_DCACHE_BASE + offset, entry, write ? 7 : 6);
    return false;
}

static bool psram_writeback_line(ESP32S3CacheState *s,
                                 ESP32S3PsramCacheLine *line)
{
    if (line->valid && line->dirty) {
        if (!esp32s3_spi_psram_cache_ready(s->spi0, s->psram, true)) {
            return false;
        }
        memcpy(psram_storage(s) + line->address, line->data,
               s->psram_line_bytes);
        memory_region_set_dirty(&s->psram->data_mr, line->address,
                                s->psram_line_bytes);
        line->dirty = false;
    }
    return true;
}

static ESP32S3PsramCacheLine *psram_cache_line(ESP32S3CacheState *s,
                                             uint32_t address)
{
    unsigned bytes = s->psram_line_bytes;
    unsigned sets = s->psram_line_count / ESP32S3_PSRAM_CACHE_WAYS;
    uint32_t tag = address & ~(bytes - 1);
    unsigned first = (tag / bytes % sets) * ESP32S3_PSRAM_CACHE_WAYS;
    ESP32S3PsramCacheLine *victim = NULL;

    for (unsigned way = 0; way < ESP32S3_PSRAM_CACHE_WAYS; way++) {
        ESP32S3PsramCacheLine *line = &s->psram_lines[first + way];
        if (line->valid && line->address == tag) {
            line->age = ++s->psram_age;
            return line;
        }
        if (!victim || !line->valid ||
            (victim->valid && line->age < victim->age)) {
            victim = line;
        }
    }
    if (!esp32s3_spi_psram_cache_ready(s->spi0, s->psram, false) ||
        !psram_writeback_line(s, victim)) {
        return NULL;
    }
    memcpy(victim->data, psram_storage(s) + tag, bytes);
    victim->address = tag;
    victim->valid = true;
    victim->dirty = false;
    victim->age = ++s->psram_age;
    return victim;
}

static bool psram_cpu_enabled(ESP32S3CacheState *s)
{
    unsigned core = current_cpu ? current_cpu->cpu_index : 0;
    return s->dcache_enable && core < 2 &&
        !(s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_DCACHE_CTRL1)] & (1 << core));
}


static MemTxResult psram_cpu_read(void *opaque, hwaddr address, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    ESP32S3CacheState *s = opaque;
    *value = 0;
    if (!psram_cpu_enabled(s) || address >= psram_capacity(s) ||
        size > psram_capacity(s) - address) {
        return MEMTX_DECODE_ERROR;
    }
    for (unsigned i = 0; i < size;) {
        ESP32S3PsramCacheLine *line = psram_cache_line(s, address + i);
        unsigned offset = (address + i) & (s->psram_line_bytes - 1);
        unsigned count = MIN(size - i, s->psram_line_bytes - offset);
        if (!line) {
            return MEMTX_DECODE_ERROR;
        }
        for (unsigned j = 0; j < count; j++) {
            *value |= (uint64_t)line->data[offset + j] << ((i + j) * 8);
        }
        i += count;
    }
    return MEMTX_OK;
}

static MemTxResult psram_cpu_write(void *opaque, hwaddr address, uint64_t value,
                                   unsigned size, MemTxAttrs attrs)
{
    ESP32S3CacheState *s = opaque;
    if (!psram_cpu_enabled(s) || address >= psram_capacity(s) ||
        size > psram_capacity(s) - address) {
        return MEMTX_DECODE_ERROR;
    }
    for (unsigned i = 0; i < size;) {
        ESP32S3PsramCacheLine *line = psram_cache_line(s, address + i);
        unsigned offset = (address + i) & (s->psram_line_bytes - 1);
        unsigned count = MIN(size - i, s->psram_line_bytes - offset);
        if (!line) {
            return MEMTX_DECODE_ERROR;
        }
        for (unsigned j = 0; j < count; j++) {
            line->data[offset + j] = value >> ((i + j) * 8);
        }
        line->dirty = true;
        i += count;
    }
    return MEMTX_OK;
}

typedef struct PsramDmaChunk {
    uint32_t physical;
    unsigned offset;
    unsigned size;
} PsramDmaChunk;

/* One SSI-backed I/O beat is at most eight bytes, hence at most two MMU pages. */
static unsigned psram_dma_chunks(ESP32S3CacheState *s, hwaddr offset,
                                  unsigned size, bool write,
                                  PsramDmaChunk chunks[2])
{
    unsigned consumed = 0, count = 0;
    if (!esp32s3_spi_psram_cache_ready(s->spi0, s->psram, write)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32-S3 unsupported or unavailable SPI0 PSRAM %s phases\n",
                      write ? "write" : "read");
        return 0;
    }
    while (consumed < size) {
        unsigned bytes = MIN(size - consumed,
            ESP32S3_PAGE_SIZE - (offset + consumed) % ESP32S3_PAGE_SIZE);
        if (!psram_translate(s, offset + consumed, bytes, write,
                             &chunks[count].physical)) {
            return 0;
        }
        chunks[count].offset = consumed;
        chunks[count].size = bytes;
        consumed += bytes;
        count++;
    }
    return count;
}

static MemTxResult psram_dma_read(void *opaque, hwaddr offset, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    ESP32S3CacheState *s = opaque;
    PsramDmaChunk chunks[2];
    unsigned count = psram_dma_chunks(s, offset, size, false, chunks);
    const uint8_t *storage = psram_storage(s);
    *value = 0;
    if (!count) {
        return MEMTX_DECODE_ERROR;
    }
    for (unsigned chunk = 0; chunk < count; chunk++) {
        const PsramDmaChunk *part = &chunks[chunk];
        for (unsigned i = 0; i < part->size; i++) {
            *value |= (uint64_t)storage[part->physical + i] <<
                      ((part->offset + i) * 8);
        }
    }
    return MEMTX_OK;
}

static MemTxResult psram_dma_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size, MemTxAttrs attrs)
{
    ESP32S3CacheState *s = opaque;
    PsramDmaChunk chunks[2];
    unsigned count = psram_dma_chunks(s, offset, size, true, chunks);
    uint8_t *storage = psram_storage(s);
    /* Validate both page translations before committing this single bus beat. */
    if (!count) {
        return MEMTX_DECODE_ERROR;
    }
    for (unsigned chunk = 0; chunk < count; chunk++) {
        const PsramDmaChunk *part = &chunks[chunk];
        for (unsigned i = 0; i < part->size; i++) {
            storage[part->physical + i] = value >> ((part->offset + i) * 8);
        }
        memory_region_set_dirty(&s->psram->data_mr, part->physical, part->size);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps psram_cpu_ops = {
    .read_with_attrs = psram_cpu_read,
    .write_with_attrs = psram_cpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static const MemoryRegionOps psram_dma_ops = {
    .read_with_attrs = psram_dma_read,
    .write_with_attrs = psram_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

/*
 * SRAM2's upper 32 KiB is DCache data SRAM; 64 KiB mode also consumes its
 * lower half. The ordinary IDF heap layout exposes only the unoccupied part.
 */
static uint8_t *sram2_data(ESP32S3CacheState *s, hwaddr offset, unsigned size)
{
    unsigned available = s->psram_line_count * s->psram_line_bytes == 65536 ?
                         0 : 32768;
    if (!s->internal_ram || offset >= available || size > available - offset) {
        return NULL;
    }
    return (uint8_t *)memory_region_get_ram_ptr(s->internal_ram) + 0x70000 + offset;
}

static MemTxResult sram2_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    uint8_t *data = sram2_data(opaque, offset, size);
    *value = 0;
    if (!data) {
        return MEMTX_DECODE_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        *value |= (uint64_t)data[i] << (i * 8);
    }
    return MEMTX_OK;
}

static MemTxResult sram2_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    ESP32S3CacheState *s = opaque;
    uint8_t *data = sram2_data(s, offset, size);
    if (!data) {
        return MEMTX_DECODE_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        data[i] = value >> (i * 8);
    }
    memory_region_set_dirty(s->internal_ram, 0x70000 + offset, size);
    return MEMTX_OK;
}

static const MemoryRegionOps sram2_ops = {
    .read_with_attrs = sram2_read,
    .write_with_attrs = sram2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void psram_sync_line(ESP32S3CacheState *s,
                           ESP32S3PsramCacheLine *line, unsigned operations)
{
    if ((operations & ESP32S3_PSRAM_WRITEBACK) &&
        !psram_writeback_line(s, line)) {
        /* Completion is not commitment: retain dirty ownership for a retry. */
        s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_ILG_INT_ST)] |=
            R_EXTMEM_CACHE_ILG_INT_ST_DCACHE_SYNC_OP_FAULT_ST_MASK;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ESP32-S3 PSRAM writeback refused by SPI0 phases\n");
        return;
    }
    if (operations & ESP32S3_PSRAM_CLEAN) {
        line->dirty = false;
    }
    if (operations & ESP32S3_PSRAM_INVALIDATE) {
        line->valid = false;
        line->dirty = false;
    }
}


void esp32s3_cache_psram_reset(ESP32S3CacheState *s)
{
    memset(s->psram_lines, 0, sizeof(s->psram_lines));
    s->psram_age = 0;
    s->psram_line_bytes = 16;
    s->psram_line_count = 32768 / 16;
}

void esp32s3_cache_psram_configure(ESP32S3CacheState *s, uint32_t ctrl)
{
    unsigned mode = (ctrl >> 3) & 3;
    unsigned bytes = 16 << MIN(mode, 2);
    unsigned capacity = (ctrl & (1 << 2)) ? 65536 : 32768;

    if (mode == 3 || (capacity == 65536 && bytes == 16)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ESP32-S3 unsupported DCache geometry 0x%x\n", ctrl);
        s->dcache_enable = false;
        return;
    }
    if (bytes != s->psram_line_bytes ||
        capacity / bytes != s->psram_line_count) {
        /* Repartitioning SRAM/tag geometry does not commit dirty data. */
        memset(s->psram_lines, 0, sizeof(s->psram_lines));
        s->psram_line_bytes = bytes;
        s->psram_line_count = capacity / bytes;
    }
    s->dcache_enable = ctrl & 1;
}

void esp32s3_cache_psram_sync(ESP32S3CacheState *s, uint32_t vaddr,
                            uint64_t size, unsigned operations)
{
    uint64_t begin, end;
    if (!s->psram) {
        return;
    }
    /* ROM Cache_Invalidate_DCache_All uses the zero-item assist command. */
    if (!size) {
        for (unsigned i = 0; i < s->psram_line_count; i++) {
            psram_sync_line(s, &s->psram_lines[i], operations);
        }
        return;
    }
    if (vaddr < ESP32S3_DCACHE_BASE ||
        vaddr >= ESP32S3_DCACHE_BASE + ESP32S3_EXTMEM_REGION_SIZE) {
        return;
    }
    begin = (vaddr - ESP32S3_DCACHE_BASE) & ~(s->psram_line_bytes - 1);
    end = MIN((uint64_t)vaddr - ESP32S3_DCACHE_BASE + size,
              ESP32S3_EXTMEM_REGION_SIZE);
    end = (end + s->psram_line_bytes - 1) & ~(s->psram_line_bytes - 1);
    for (uint64_t page = begin / ESP32S3_PAGE_SIZE;
         page * ESP32S3_PAGE_SIZE < end; page++) {
        ESP32S3MMUEntry entry = s->mmu[page];
        uint64_t page_start = page * ESP32S3_PAGE_SIZE;
        uint64_t physical = (uint64_t)entry.page_number * ESP32S3_PAGE_SIZE;
        uint64_t low = physical + MAX(begin, page_start) - page_start;
        uint64_t high = physical + MIN(end, page_start + ESP32S3_PAGE_SIZE) - page_start;
        if (entry.invalid || entry.type != ESP32S3_MMU_TYPE_PSRAM ||
            high > psram_capacity(s)) {
            continue;
        }
        for (unsigned i = 0; i < s->psram_line_count; i++) {
            ESP32S3PsramCacheLine *line = &s->psram_lines[i];
            if (!line->valid || line->address < low ||
                line->address >= high) {
                continue;
            }
            psram_sync_line(s, line, operations);
        }
    }
}

void esp32s3_cache_psram_init(ESP32S3CacheState *s)
{
    memory_region_init_io(&s->sram2, OBJECT(s), &sram2_ops, s,
                          "esp32s3.sram2.cache-ownership", 65536);
    if (s->psram) {
        memory_region_init_io(&s->psram_cpu, OBJECT(s), &psram_cpu_ops, s,
                              "esp32s3.psram.cpu-cache", psram_capacity(s));
        /* Committed PSRAM bytes, resolvable from address_space_memory at the
         * private alias base that the cache MMU translation targets. */
        memory_region_init_alias(&s->psram_phys, OBJECT(s), "esp32s3.psram.phys",
                                 &s->psram_cpu, 0, psram_capacity(s));
    }
    /* An absent chip still gets a faulting external DMA window, not RAM. */
    memory_region_init_io(&s->psram_dma, OBJECT(s), &psram_dma_ops, s,
                          "esp32s3.psram.edma-mmu", ESP32S3_EXTMEM_REGION_SIZE);
    s->psram_regions_ready = true;
}
