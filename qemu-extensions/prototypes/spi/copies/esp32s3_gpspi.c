/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_gpspi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"

#define CMD 0x00
#define ADDR 0x04
#define CTRL 0x08
#define CLOCK 0x0c
#define USER 0x10
#define USER1 0x14
#define USER2 0x18
#define DLEN 0x1c
#define MISC 0x20
#define DMA 0x30
#define ENA 0x34
#define CLR 0x38
#define RAW 0x3c
#define ST 0x40
#define SET 0x44
#define W0 0x98
#define SLAVE 0xe0
#define CLK_GATE 0xe8
#define DATE 0xf0
#define R(s, a) ((s)->reg[(a) / 4])
#define A(s, a) ((s)->active[(a) / 4])
#define USR BIT(24)
#define DONE BIT(12)
#define TX_EMPTY BIT(18)
#define RX_FULL BIT(17)
#define INT_MASK 0x1fffff

static void irq_update(ESP32S3GpSpiState *s)
{
    qemu_set_irq(s->irq, !!(s->raw & s->ena));
}
static Clock *source(ESP32S3GpSpiState *s)
{
    return A(s, CLK_GATE) & BIT(2) ? s->pll80 : s->xtal;
}
static void dependency_stop(void *opaque)
{
    ESP32S3GpSpiState *s = opaque;
    if (s->waiting) {
        vm_stop(RUN_STATE_PAUSED);
    }
}
static void pause_dependency(ESP32S3GpSpiState *s, const char *reason)
{
    s->waiting = true;
    timer_del(s->timer);
    qemu_log_mask(LOG_GUEST_ERROR, "SPI%u strict dependency: %s at %" PRId64 " ns\n",
                  s->controller + 2, reason, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    /* Stop only from main context, after timer/qtest dispatch releases its
     * timer-list locks. BUSY/partial data already remain held above. */
    qemu_bh_schedule(s->pause_bh);
}
static bool drive(ESP32S3GpSpiState *s, bool clk, bool data_oe)
{
    if (!s->provider.drive) {
        /* The hardware shifter still runs without an attached graph. Reads
         * cannot manufacture input, and will stop at their sampling edge. */
        return true;
    }
    if (!s->provider.drive(s->provider.opaque, s->controller, clk, s->mosi,
                          data_oe, s->cs_mask, s->cs_polarity)) {
        pause_dependency(s, "unresolved driven clock/MOSI/CS topology");
        return false;
    }
    return true;
}
static void abort_transfer(ESP32S3GpSpiState *s)
{
    timer_del(s->timer);
    qemu_bh_cancel(s->pause_bh);
    if (s->provider.end) {
        s->provider.end(s->provider.opaque, s->controller, true);
    }
    s->busy = s->waiting = false;
    s->phase = S3_SPI_IDLE;
    s->cs_mask = 0;
    R(s, CMD) &= ~USR;
}
void esp32s3_gpspi_bind(ESP32S3GpSpiState *s, const S3SPIProvider *provider)
{
    abort_transfer(s);
    s->provider = *provider;
}
static void schedule(ESP32S3GpSpiState *s, uint64_t ticks)
{
    s->ticks += ticks;
    int64_t next = s->epoch_ns + clock_ticks_to_ns(source(s), s->ticks) / 2;
    timer_mod(s->timer, MAX(next, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1));
}
static GdmaPeripheral peripheral(ESP32S3GpSpiState *s)
{
    return s->controller ? GDMA_SPI3 : GDMA_SPI2;
}
static bool dma_byte(ESP32S3GpSpiState *s, bool rx, uint8_t *byte)
{
    unsigned dir = rx ? ESP_GDMA_IN_IDX : ESP_GDMA_OUT_IDX;
    uint32_t chan = rx ? s->rx_channel : s->tx_channel;
    if (!s->gdma || chan >= ESP_GDMA_GET_CLASS(s->gdma)->m_channel_count) {
        return false;
    }
    DmaConfigState *st = &s->gdma->ch_conf[dir][chan];
    if (!st->fsm_active || st->halted || st->peripheral != peripheral(s)) {
        return false;
    }
    /* This lane deliberately qualifies internal RAM only. External cache
     * aliases are not proof of an EDMA/PSRAM reachable memory path. */
    if (st->cur_buf_addr >= 0x3c000000 && st->cur_buf_addr < 0x3e000000) {
        pause_dependency(s, "PSRAM DMA path is unsupported/unqualified");
        return false;
    }
    return rx ? esp_gdma_write_channel(s->gdma, chan, byte, 1) :
                esp_gdma_read_channel(s->gdma, chan, byte, 1);
}
static void fifo_error(ESP32S3GpSpiState *s, bool rx)
{
    s->raw |= rx ? RX_FULL : TX_EMPTY;
    irq_update(s);
    pause_dependency(s, rx ? "RX GDMA unavailable/short/owner or address error" :
                             "TX GDMA unavailable/short/owner or address error");
}
static bool tx_phase(ESP32S3GpSpiState *s)
{
    return s->phase == S3_SPI_TX && s->tx_enabled;
}
static bool mosi_output(ESP32S3GpSpiState *s)
{
    return tx_phase(s) || s->phase == S3_SPI_COMMAND ||
           s->phase == S3_SPI_ADDRESS ||
           (s->phase == S3_SPI_DUMMY && (A(s, CTRL) & BIT(3)));
}
static bool rx_phase(ESP32S3GpSpiState *s)
{
    return s->rx_enabled && (s->phase == S3_SPI_RX ||
                             (s->phase == S3_SPI_TX && s->duplex));
}
static bool prepare_bit(ESP32S3GpSpiState *s)
{
    unsigned i = s->bit;
    switch (s->phase) {
    case S3_SPI_COMMAND:
        s->mosi = (A(s, USER2) >> ((i / 8) * 8 + (s->tx_lsb ? i % 8 : 7 - i % 8))) & 1;
        break;
    case S3_SPI_ADDRESS:
        s->mosi = (A(s, ADDR) >> (s->tx_lsb ? 24 - (i / 8) * 8 + i % 8 : 31 - i)) & 1;
        break;
    case S3_SPI_TX:
        if (!s->tx_enabled) {
            s->mosi = !!(A(s, CTRL) & BIT(19));
            break;
        }
        if (!(i % 8)) {
            if (s->tx_dma) {
                if (!dma_byte(s, false, &s->tx_byte)) {
                    if (!s->waiting) { fifo_error(s, false); }
                    return false;
                }
            } else {
                unsigned base = A(s, USER) & BIT(25) ? 32 : 0;
                unsigned offset = base + i / 8;
                s->tx_byte = A(s, W0 + (offset / 4) * 4) >> ((offset % 4) * 8);
            }
        }
        s->mosi = (s->tx_byte >> (s->tx_lsb ? i % 8 : 7 - i % 8)) & 1;
        break;
    default:
        s->mosi = !!(A(s, CTRL) & BIT(19));
        break;
    }
    return true;
}
static bool sample_bit(ESP32S3GpSpiState *s)
{
    if (!rx_phase(s)) {
        return true;
    }
    bool value;
    if (!s->provider.sample ||
        !s->provider.sample(s->provider.opaque, s->controller, &value)) {
        pause_dependency(s, "MISO disconnected/floating/contended/unknown");
        return false;
    }
    unsigned shift = s->rx_lsb ? s->bit % 8 : 7 - s->bit % 8;
    if (!(s->bit % 8)) { s->rx_byte = 0; }
    s->rx_byte |= value << shift;
    if ((s->bit % 8) == 7 || s->bit + 1 == s->bits) {
        if (s->rx_dma) {
            DmaConfigState *st = &s->gdma->ch_conf[ESP_GDMA_IN_IDX][s->rx_channel];
            /* In full duplex MS_DLEN clocks TX length; the IDF RX inlink
             * may intentionally retain only a shorter prefix. A completely
             * consumed, clean terminal inlink is not an RX descriptor fault.
             * Continue actual MISO sampling but do not write beyond it. */
            bool prefix_complete = s->duplex && !st->fsm_active && !st->halted &&
                st->peripheral == peripheral(s) && st->rx_last_desc_addr &&
                !st->cur_next_addr && st->cur_buf_off == st->cur_buf_cap;
            if (!prefix_complete && !dma_byte(s, true, &s->rx_byte)) {
                if (!s->waiting) { fifo_error(s, true); }
                return false;
            }
        } else {
            unsigned base = A(s, USER) & BIT(24) ? 32 : 0;
            unsigned offset = base + s->bit / 8;
            uint32_t mask = 0xffu << ((offset % 4) * 8);
            R(s, W0 + (offset / 4) * 4) =
                (R(s, W0 + (offset / 4) * 4) & ~mask) |
                ((uint32_t)s->rx_byte << ((offset % 4) * 8));
        }
    }
    return true;
}
static void complete(ESP32S3GpSpiState *s)
{
    if (s->rx_dma && s->rx_enabled &&
        !esp_gdma_finish_rx_channel(s->gdma, s->rx_channel)) {
        fifo_error(s, true);
        return;
    }
    if (!(A(s, MISC) & BIT(30))) {
        s->cs_mask = 0;
        if (!drive(s, s->cpol, false)) { return; }
        if (s->provider.end) { s->provider.end(s->provider.opaque, s->controller, false); }
    }
    s->busy = false;
    s->phase = S3_SPI_IDLE;
    R(s, CMD) &= ~USR;
    s->raw |= DONE;
    irq_update(s);
}
static bool next_phase(ESP32S3GpSpiState *s)
{
    uint32_t user = A(s, USER);
    do {
        ++s->phase;
        s->bit = 0;
        switch (s->phase) {
        case S3_SPI_COMMAND: s->bits = user & BIT(31) ? ((A(s, USER2) >> 28) & 15) + 1 : 0; break;
        case S3_SPI_ADDRESS: s->bits = user & BIT(30) ? ((A(s, USER1) >> 27) & 31) + 1 : 0; break;
        case S3_SPI_DUMMY: s->bits = user & BIT(29) ? (A(s, USER1) & 255) + 1 : 0; break;
        case S3_SPI_TX: s->bits = s->tx_enabled || (s->duplex && s->rx_enabled) ? s->data_bits : 0; break;
        case S3_SPI_RX: s->bits = !s->duplex && s->rx_enabled ? s->data_bits : 0; break;
        case S3_SPI_HOLD: {
            unsigned hold = (A(s, USER1) >> 22) & 31;
            if ((user & BIT(6)) && hold) {
                schedule(s, s->period_ticks * hold);
            } else if (!s->ticks) {
                /* Even an empty launch retires on a module-clock boundary;
                 * USR must not synthesize same-MMIO-call TRANS_DONE. */
                schedule(s, 2);
            } else { complete(s); }
            return false;
        }
        default: return false;
        }
    } while (!s->bits);
    s->leading = true;
    if (!s->cpha && !prepare_bit(s)) { return false; }
    if (!drive(s, s->cpol, mosi_output(s))) { return false; }
    schedule(s, s->cpol ? s->high_ticks : s->low_ticks);
    return true;
}
static void tick(void *opaque)
{
    ESP32S3GpSpiState *s = opaque;
    if (!s->busy || s->waiting) { return; }
    if (s->phase == S3_SPI_SETUP) { next_phase(s); return; }
    if (s->phase == S3_SPI_HOLD) { complete(s); return; }
    bool clk = s->leading ? !s->cpol : s->cpol;
    bool data_oe = mosi_output(s);
    if (s->leading && s->cpha && !prepare_bit(s)) { return; }
    if (!(s->phase == S3_SPI_DUMMY && (A(s, USER) & BIT(26)))) {
        if (!drive(s, clk, data_oe)) { return; }
    }
    if (s->leading != s->cpha && !sample_bit(s)) { return; }
    if (!s->leading) {
        if (++s->bit == s->bits) { next_phase(s); return; }
        if (!s->cpha && !prepare_bit(s)) { return; }
        if (!drive(s, s->cpol, data_oe)) { return; }
    }
    s->leading = !s->leading;
    schedule(s, clk ? s->high_ticks : s->low_ticks);
}
static void start(ESP32S3GpSpiState *s)
{
    if (s->busy) { return; }
    memcpy(s->active, s->reg, sizeof(s->active));
    s->busy = true;
    s->waiting = false;
    R(s, CMD) |= USR;
    s->raw &= ~DONE;
    irq_update(s);
    if (!s->gate || s->reset_asserted || (A(s, CLK_GATE) & 3) != 3 ||
        !clock_get(source(s))) {
        pause_dependency(s, "clock gated/reset/source stopped"); return;
    }
    if ((A(s, SLAVE) & (BIT(26) | BIT(28))) ||
        (A(s, CTRL) & (0x7e0 | 0x1c000)) ||
        (A(s, USER) & (BIT(3) | BIT(4) | BIT(12) | BIT(13) | BIT(14) | BIT(15) | BIT(17))) ||
        (A(s, MISC) & (0xf0000 | BIT(31))) || (A(s, DMA) & BIT(18)) ||
        ((A(s, CTRL) >> 23) & 3) > 1 || ((A(s, CTRL) >> 25) & 3) > 1) {
        pause_dependency(s, "slave/multiline/DDR/3wire/segmented/reserved mode unsupported"); return;
    }
    s->tx_enabled = A(s, USER) & BIT(27);
    s->rx_enabled = A(s, USER) & BIT(28);
    s->duplex = A(s, USER) & 1;
    if (!s->duplex && s->tx_enabled && s->rx_enabled) {
        pause_dependency(s, "ESP32-S3 half-duplex simultaneous TX/RX data phases unsupported");
        return;
    }
    s->tx_dma = A(s, DMA) & BIT(28);
    s->rx_dma = A(s, DMA) & BIT(27);
    s->tx_lsb = A(s, CTRL) & BIT(25);
    s->rx_lsb = A(s, CTRL) & BIT(23);
    s->data_bits = (A(s, DLEN) & 0x3ffff) + 1;
    unsigned tx_capacity = A(s, USER) & BIT(25) ? 256 : 512;
    unsigned rx_capacity = A(s, USER) & BIT(24) ? 256 : 512;
    if ((s->tx_enabled && !s->tx_dma && s->data_bits > tx_capacity) ||
        (s->rx_enabled && !s->rx_dma && s->data_bits > rx_capacity)) {
        pause_dependency(s, "PIO length exceeds selected 64/32byte hardware buffer"); return;
    }
    s->tx_channel = s->rx_channel = UINT32_MAX;
    if (s->tx_dma && s->tx_enabled &&
        (!s->gdma || !esp_gdma_get_channel_periph(s->gdma, peripheral(s), ESP_GDMA_OUT_IDX, &s->tx_channel))) {
        fifo_error(s, false); return;
    }
    if (s->rx_dma && s->rx_enabled &&
        (!s->gdma || !esp_gdma_get_channel_periph(s->gdma, peripheral(s), ESP_GDMA_IN_IDX, &s->rx_channel))) {
        fifo_error(s, true); return;
    }
    s->cpol = A(s, MISC) & BIT(29);
    s->cpha = s->cpol ^ !!(A(s, USER) & BIT(9));
    s->cs_mask = (~A(s, MISC)) & (s->controller ? 7 : 63);
    s->cs_polarity = (A(s, MISC) >> 7) & 63;
    if (A(s, MISC) & BIT(6)) {
        pause_dependency(s, "SCLK output disabled"); return;
    }
    uint32_t cfg = A(s, CLOCK);
    unsigned pre = ((cfg >> 18) & 15) + 1;
    if (cfg & BIT(31)) {
        /* Equal-source clock uses half-source periods below by a doubled
         * tick representation, retaining a full divider-one clock. */
        s->high_ticks = s->low_ticks = 1;
        s->period_ticks = 2;
    } else {
        s->period_ticks = 2 * pre * (((cfg >> 12) & 63) + 1);
        s->high_ticks = 2 * pre * (((cfg >> 6) & 63) + 1);
        if (s->high_ticks >= s->period_ticks) {
            pause_dependency(s, "invalid SCLK duty/divider fields"); return;
        }
        s->low_ticks = s->period_ticks - s->high_ticks;
    }
    s->ticks = 0;
    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->phase = S3_SPI_SETUP;
    s->mosi = true;
    if (!drive(s, s->cpol, false)) { return; }
    if (A(s, USER) & BIT(7)) {
        schedule(s, s->period_ticks * (((A(s, USER1) >> 17) & 31) + 1));
    } else { next_phase(s); }
}
static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3GpSpiState *s = opaque;
    switch (addr) {
    case RAW: return s->raw;
    case ST: return s->raw & s->ena;
    case ENA: return s->ena;
    case CLR: case SET: return 0;
    default: return addr < sizeof(s->reg) ? R(s, addr) : 0;
    }
}
static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3GpSpiState *s = opaque;
    uint32_t val = value;
    if (s->reset_asserted) { return; }
    switch (addr) {
    case ENA: s->ena = val & INT_MASK; irq_update(s); return;
    case CLR: case RAW: s->raw &= ~(val & INT_MASK); irq_update(s); return;
    case SET: s->raw |= val & INT_MASK; irq_update(s); return;
    case ST: return;
    case CMD:
        /* UPDATE crosses register domains, not a transfer completion. */
        R(s, CMD) = (val & ~(BIT(23) | USR)) | (s->busy ? USR : 0);
        if (val & USR) { start(s); }
        return;
    case SLAVE:
        R(s, addr) = val & ~BIT(27);
        if (val & BIT(27)) { abort_transfer(s); }
        return;
    case CLK_GATE:
        if ((s->busy || s->cs_mask) && val != R(s, addr)) { abort_transfer(s); }
        R(s, addr) = val & 7;
        return;
    case DMA:
        if (s->busy && (val & 0xe0000000)) { abort_transfer(s); }
        R(s, addr) = val & ~0xe0000000;
        return;
    default:
        if (addr < sizeof(s->reg)) { R(s, addr) = val; }
        return;
    }
}
static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static void reset_hold(Object *obj, ResetType type)
{
    ESP32S3GpSpiState *s = ESP32S3_GPSPI(obj);
    if (s->soc_reset && !esp32s3_reset_covers_periph(s->soc_reset)) { return; }
    abort_transfer(s);
    memset(s->reg, 0, sizeof(s->reg));
    R(s, CLOCK) = BIT(31);
    R(s, USER) = BIT(31);
    R(s, USER1) = 23u << 27;
    R(s, USER2) = 7u << 28;
    R(s, DATE) = 0x2101190;
    s->raw = s->ena = 0;
    irq_update(s);
}
static void gate_input(void *opaque, int n, int level)
{
    ESP32S3GpSpiState *s = opaque;
    s->gate = level;
    if (!level) { abort_transfer(s); }
}
static void reset_input(void *opaque, int n, int level)
{
    ESP32S3GpSpiState *s = opaque;
    s->reset_asserted = level;
    if (level) { device_cold_reset(DEVICE(s)); }
}
static void xtal_changed(void *opaque, ClockEvent event)
{
    ESP32S3GpSpiState *s = opaque;
    if ((s->busy || s->cs_mask) && !(A(s, CLK_GATE) & BIT(2))) {
        abort_transfer(s);
    }
}
static void pll_changed(void *opaque, ClockEvent event)
{
    ESP32S3GpSpiState *s = opaque;
    if ((s->busy || s->cs_mask) && (A(s, CLK_GATE) & BIT(2))) {
        abort_transfer(s);
    }
}
static void init(Object *obj)
{
    ESP32S3GpSpiState *s = ESP32S3_GPSPI(obj);
    memory_region_init_io(&s->iomem, obj, &ops, s, TYPE_ESP32S3_GPSPI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    s->pause_bh = qemu_bh_new(dependency_stop, s);
    s->xtal = qdev_init_clock_in(DEVICE(obj), "xtal-clk", xtal_changed, s, ClockUpdate);
    s->pll80 = qdev_init_clock_in(DEVICE(obj), "pll-f80m-clk", pll_changed, s, ClockUpdate);
    s->gate = true;
    qdev_init_gpio_in_named(DEVICE(obj), gate_input, "clk-gate", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_input, "reset", 1);
}
static void finalize(Object *obj)
{
    ESP32S3GpSpiState *s = ESP32S3_GPSPI(obj);
    timer_free(s->timer);
    qemu_bh_delete(s->pause_bh);
}
static Property props[] = {
    DEFINE_PROP_UINT32("controller", ESP32S3GpSpiState, controller, 0),
    DEFINE_PROP_LINK("soc-reset", ESP32S3GpSpiState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};
static void class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    RESETTABLE_CLASS(klass)->phases.hold = reset_hold;
    device_class_set_props(dc, props);
}
static const TypeInfo type = {
    .name = TYPE_ESP32S3_GPSPI, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3GpSpiState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&type); }
type_init(register_types)
