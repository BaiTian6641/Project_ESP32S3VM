/* SPDX-License-Identifier: GPL-2.0-or-later
 * ESP32-S3 UHCI0, TRM 26.4.11 / 26.7.2 and pinned IDF6.1 uhci_ll.h.
 * UART FIFO callbacks are the sole byte transport; console chardevs are unrelated.
 */
#include "qemu/osdep.h"
#include "hw/char/esp32s3_uhci.h"
#include "hw/qdev-properties.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bitops.h"

#define R(s, a) ((s)->reg[(a) / 4])
#define B(n) (1u << (n))
#define QUANTUM 256
#define CONF_MASK 0x1fff
#define INT_MASK 0x1ff

static void irq_update(ESP32S3UhciState *s)
{
    qemu_set_irq(s->irq, s->gate && !s->reset_asserted &&
                 (R(s, 4) & R(s, 0x0c)));
}

static int selected(ESP32S3UhciState *s)
{
    unsigned mask = (R(s, 0) >> 2) & 7;
    if (!mask || (mask & (mask - 1))) {
        return -1;
    }
    return ctz32(mask);
}

static bool supported(ESP32S3UhciState *s)
{
    /* The target sources do not specify the header/CRC or quick-send wire
     * layout. Do not manufacture packets for undocumented configurations. */
    if ((R(s, 0) & (B(6) | B(7) | B(10))) ||
        (R(s, 0x34) & (B(3) | B(7)))) {
        if (!s->diagnostic) {
            qemu_log_mask(LOG_UNIMP, "UHCI0: header/CRC/quick-send wire format "
                          "requires target specification; transfer suspended\n");
            s->diagnostic = true;
        }
        return false;
    }
    return true;
}

static void codec_reset(ESP32S3UhciState *s, bool tx, bool rx)
{
    if (tx) {
        s->tx_count = s->tx_offset = 0;
        s->tx_open = s->tx_close = false;
        s->tx_bound = false;
    }
    if (rx) {
        s->rx_length = 0;
        s->rx_have_pending = s->rx_have_prefix = s->rx_started = false;
        s->rx_bound = s->rx_end_pending = false;
        s->rx_error = s->rx_boundary_pending = false;
        s->rx_raw_sequence = 0;
        s->rx_event_head = s->rx_event_count = 0;
        R(s, 0x1c) = 0;
    }
    if (!s->tx_open && !s->rx_started && !s->tx_count && !s->rx_event_count) {
        s->packet_uart = -1;
    }
}

static bool rx_finish(ESP32S3UhciState *s)
{
    bool (*finish)(ESPGdmaState *, uint32_t) = s->rx_error ?
        esp_gdma_finish_rx_channel_err : esp_gdma_finish_rx_channel;
    if (s->rx_bound && s->rx_length && !finish(s->gdma, s->rx_channel)) {
        return false;
    }
    s->rx_length = 0;
    s->rx_started = s->rx_bound = s->rx_end_pending = false;
    s->rx_have_prefix = false;
    if (s->rx_boundary_pending) {
        s->rx_event_head = (s->rx_event_head + 1) % S3_UHCI_RX_BOUNDARIES;
        s->rx_event_count--;
    }
    s->rx_error = s->rx_boundary_pending = false;
    return true;
}

static bool rx_step(ESP32S3UhciState *s, S3UhciUart *u)
{
    uint32_t channel;
    uint8_t byte;
    if (R(s, 0) & B(1)) {
        return false;
    }
    if (s->rx_event_count && !s->rx_have_pending) {
        unsigned head = s->rx_event_head;
        uint64_t end = s->rx_event_end[head];
        bool delimiter = s->rx_event_delimiter[head];
        if (s->rx_raw_sequence >= end - delimiter) {
            if (delimiter && s->rx_raw_sequence < end) {
                if (!u->rx_used(u->opaque) || !u->read_byte(u->opaque, &byte)) {
                    return false;
                }
                s->rx_raw_sequence++;
                /* NULL is qualified by the real UART's all-low frame detector,
                 * not inferred from arbitrary zero-valued payload data. */
                if (byte != 0) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "UHCI0: break FIFO boundary lost its NULL frame\\n");
                    s->rx_error = true;
                }
            }
            s->rx_error |= s->rx_have_prefix;
            s->rx_boundary_pending = s->rx_end_pending = true;
        }
    }
    if (s->rx_end_pending) {
        return rx_finish(s);
    }
    if (!esp_gdma_get_channel_periph(s->gdma, GDMA_UHCI0,
                                    ESP_GDMA_IN_IDX, &channel)) {
        return false;
    }
    /* A packet cannot migrate to another IN channel when its owner stops. */
    if (s->rx_bound && channel != s->rx_channel) {
        return false;
    }
    if (s->rx_have_pending) {
        byte = s->rx_pending;
        uint32_t served;
        esp_gdma_write_channel_ex(s->gdma, channel, &byte, 1, &served);
        if (!served) {
            return false;
        }
        s->rx_have_pending = false;
        s->rx_bound = true;
        s->rx_channel = channel;
        s->rx_length++;
        if ((R(s, 0) & B(9)) && R(s, 0x80) &&
            s->rx_length >= R(s, 0x80)) {
            s->rx_end_pending = true;
        }
        return true;
    }
    if (!u->rx_used(u->opaque) || !u->read_byte(u->opaque, &byte)) {
        return false;
    }
    s->rx_raw_sequence++;
    if ((R(s, 0) & B(5)) && byte == (uint8_t)R(s, 0x70)) {
        R(s, 4) |= B(1); /* TRM: TX_START is separator detected. */
        if (s->rx_have_prefix) {
            /* A delimiter aborts a truncated escape; never emit its prefix. */
            s->rx_error = true;
            s->rx_have_prefix = false;
        }
        if (s->rx_length && (!(R(s, 0) & B(9)) || s->rx_error)) {
            s->rx_end_pending = true;
        } else {
            s->rx_started = true;
        }
        return true;
    }
    if ((R(s, 0) & B(5)) && !s->rx_started) {
        return true; /* Ignore bytes outside a framed packet. */
    }
    if (s->rx_have_prefix) {
        bool decoded = false;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t esc = R(s, 0x70 + 4 * i);
            if ((R(s, 0x24) & B(4 + i)) &&
                s->rx_prefix == (uint8_t)(esc >> 8) &&
                byte == (uint8_t)(esc >> 16)) {
                byte = esc;
                decoded = true;
                break;
            }
        }
        s->rx_have_prefix = false;
        if (!decoded) {
            s->rx_error = true;
            s->rx_end_pending = true;
            return true;
        }
    } else {
        for (unsigned i = 0; i < 4; i++) {
            uint32_t esc = R(s, 0x70 + 4 * i);
            if ((R(s, 0x24) & B(4 + i)) && byte == (uint8_t)(esc >> 8)) {
                s->rx_prefix = byte;
                s->rx_have_prefix = true;
                return true;
            }
        }
    }
    s->rx_started = true;
    s->rx_pending = byte;
    s->rx_have_pending = true;
    return true;
}

static bool tx_step(ESP32S3UhciState *s, S3UhciUart *u)
{
    uint32_t channel;
    uint8_t byte;
    if ((R(s, 0) & B(0)) || !u->tx_free(u->opaque)) {
        return false;
    }
    if (s->tx_offset < s->tx_count) {
        if (!u->write_byte(u->opaque, s->tx_pending[s->tx_offset])) {
            return false;
        }
        s->tx_offset++;
        if (s->tx_offset == s->tx_count) {
            s->tx_count = s->tx_offset = 0;
        }
        return true;
    }
    if (s->tx_close) {
        if ((R(s, 0) & B(5)) &&
            !u->write_byte(u->opaque, R(s, 0x70))) {
            return false;
        }
        s->tx_close = s->tx_open = false;
        s->tx_bound = false;
        return true;
    }
    if ((R(s, 0x18) & B(7)) && !(R(s, 0x18) & B(8))) {
        return false;
    }
    if (!esp_gdma_get_channel_periph(s->gdma, GDMA_UHCI0,
                                    ESP_GDMA_OUT_IDX, &channel)) {
        return false;
    }
    if (s->tx_bound && s->tx_channel != channel) {
        return false;
    }
    s->tx_bound = true;
    s->tx_channel = channel;
    EspGdmaTxInfo info = { 0 };
    if (!s->tx_open && (R(s, 0) & B(5))) {
        if (!u->write_byte(u->opaque, R(s, 0x70))) {
            return false;
        }
        s->tx_open = true;
        R(s, 4) |= B(0); /* TRM: RX_START is separator sent. */
        return true;
    }
    esp_gdma_read_channel_ex(s->gdma, channel, &byte, 1, &info);
    if (!info.served) {
        s->tx_close = info.eof_reached || info.list_done;
        return s->tx_close;
    }
    s->tx_open = true;
    s->tx_pending[0] = byte;
    s->tx_count = 1;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t esc = R(s, 0x70 + 4 * i);
        if ((R(s, 0x24) & B(i)) && byte == (uint8_t)esc) {
            s->tx_pending[0] = esc >> 8;
            s->tx_pending[1] = esc >> 16;
            s->tx_count = 2;
            break;
        }
    }
    s->tx_close = info.eof_reached;
    if (info.list_done && !info.eof_reached) {
        /* A terminated framed outlink without suc_eof is a genuine EOF error. */
        if (R(s, 0) & B(5)) {
            R(s, 4) |= B(6);
        }
        s->tx_close = true;
    }
    if (info.eof_reached) {
        R(s, 0x18) &= ~B(8);
    }
    return true;
}

void esp32s3_uhci_service(ESP32S3UhciState *s)
{
    int uart = selected(s);
    if (s->servicing || !s->gdma || !s->gate || s->reset_asserted || uart < 0 ||
        !supported(s) || (s->packet_uart >= 0 && uart != s->packet_uart)) {
        return;
    }
    S3UhciUart *u = &s->uart[uart];
    if (!u->read_byte || !u->write_byte || !u->rx_used || !u->tx_free) {
        return;
    }
    s->servicing = true;
    s->notified = false;
    unsigned n;
    for (n = 0; n < QUANTUM; n++) {
        bool progress = rx_step(s, u);
        progress |= tx_step(s, u);
        if (!progress) {
            break;
        }
        s->packet_uart = uart;
    }
    if (!s->tx_open && !s->tx_count && !s->rx_started &&
        !s->rx_have_pending && !s->rx_event_count) {
        s->packet_uart = -1;
    }
    irq_update(s);
    s->servicing = false;
    if (n == QUANTUM || s->notified) {
        qemu_bh_schedule(s->bh);
    }
}

void esp32s3_uhci_kick(void *opaque)
{
    ESP32S3UhciState *s = opaque;
    if (s->servicing) {
        s->notified = true;
    } else if (s->gate && !s->reset_asserted) {
        qemu_bh_schedule(s->bh);
    }
}

void esp32s3_uhci_attach_uart(ESP32S3UhciState *s, unsigned uart,
                            const S3UhciUart *ops)
{
    assert(uart < ARRAY_SIZE(s->uart));
    s->uart[uart] = *ops;
    esp32s3_uhci_kick(s);
}

void esp32s3_uhci_rx_event(ESP32S3UhciState *s, unsigned uart,
                         S3UhciRxEvent event, bool delimiter_queued,
                         unsigned queued_bytes)
{
    if (selected(s) != uart || !s->gate || s->reset_asserted ||
        (s->packet_uart >= 0 && s->packet_uart != uart)) {
        return;
    }
    if (!((event == S3_UHCI_RX_IDLE && (R(s, 0) & B(8))) ||
          (event == S3_UHCI_RX_BREAK && (R(s, 0) & B(12))))) {
        return;
    }
    if (queued_bytes >= S3_UHCI_RX_BOUNDARIES ||
        (delimiter_queued && (event != S3_UHCI_RX_BREAK || !queued_bytes))) {
        qemu_log_mask(LOG_GUEST_ERROR, "UHCI0: invalid physical RX boundary\\n");
        return;
    }
    uint64_t end = s->rx_raw_sequence + queued_bytes;
    if (s->rx_event_count) {
        unsigned tail = (s->rx_event_head + s->rx_event_count - 1) %
                        S3_UHCI_RX_BOUNDARIES;
        if (end == s->rx_event_end[tail]) {
            s->rx_event_delimiter[tail] |= delimiter_queued;
            return;
        }
        if (end < s->rx_event_end[tail]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "UHCI0: physical RX boundary moved behind queued data\\n");
            return;
        }
    }
    if (s->rx_event_count == S3_UHCI_RX_BOUNDARIES) {
        qemu_log_mask(LOG_GUEST_ERROR, "UHCI0: physical RX boundary queue full\\n");
        return;
    }
    unsigned slot = (s->rx_event_head + s->rx_event_count) % S3_UHCI_RX_BOUNDARIES;
    s->rx_event_end[slot] = end;
    s->rx_event_delimiter[slot] = delimiter_queued;
    s->rx_event_count++;
    s->packet_uart = uart;
    esp32s3_uhci_kick(s);
}

static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3UhciState *s = opaque;
    if (addr >= sizeof(s->reg)) {
        return 0;
    }
    switch (addr) {
    case 8: return R(s, 4) & R(s, 0x0c);
    case 0x10: case 0x14: return 0;
    default: return R(s, addr);
    }
}

static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3UhciState *s = opaque;
    if (addr >= sizeof(s->reg) || s->reset_asserted) {
        return;
    }
    switch (addr) {
    case 0:
        codec_reset(s, value & B(0), value & B(1));
        R(s, 0) = value & CONF_MASK;
        s->diagnostic = false;
        break;
    case 4:
        R(s, 4) = (R(s, 4) & ~0x180) | (value & 0x180);
        break;
    case 8: case 0x1c: case 0x20: case 0x30:
        return;
    case 0x0c: R(s, addr) = value & INT_MASK; break;
    case 0x10: R(s, 4) &= ~(value & INT_MASK); break;
    case 0x14: R(s, 4) |= (value & 3) << 7; break;
    case 0x18: R(s, addr) = value & 0x1bf; break;
    case 0x24: R(s, addr) = value & 0xff; break;
    case 0x28: R(s, addr) = value & 0xffffff; break;
    case 0x2c: R(s, addr) = value & 7; break;
    case 0x34: R(s, addr) = value & 0xff; s->diagnostic = false; break;
    case 0x70: case 0x74: case 0x78: case 0x7c:
        R(s, addr) = value & 0xffffff; break;
    case 0x80: R(s, addr) = value & 0x1fff; break;
    default: R(s, addr) = value; break;
    }
    irq_update(s);
    esp32s3_uhci_kick(s);
}

static void reset_registers(ESP32S3UhciState *s)
{
    qemu_bh_cancel(s->bh);
    memset(s->reg, 0, sizeof(s->reg));
    R(s, 0) = 0x6e0;
    R(s, 0x18) = 0x33;
    R(s, 0x24) = 0x33;
    R(s, 0x28) = 0x810810;
    R(s, 0x70) = 0xdcdbc0;
    R(s, 0x74) = 0xdddbdb;
    R(s, 0x78) = 0xdedb11;
    R(s, 0x7c) = 0xdfdb13;
    R(s, 0x80) = 128;
    R(s, 0x84) = 0x02010090;
    codec_reset(s, true, true);
    s->packet_uart = -1;
    s->diagnostic = false;
    irq_update(s);
}

static void reset_hold(Object *obj, ResetType type)
{
    ESP32S3UhciState *s = ESP32S3_UHCI(obj);
    if (!s->soc_reset || esp32s3_reset_covers_periph(s->soc_reset)) {
        reset_registers(s);
    }
}

static void gate_input(void *opaque, int n, int level)
{
    ESP32S3UhciState *s = opaque;
    s->gate = !!level;
    if (!level) {
        qemu_bh_cancel(s->bh);
    } else {
        esp32s3_uhci_kick(s);
    }
    irq_update(s);
}

static void reset_input(void *opaque, int n, int level)
{
    ESP32S3UhciState *s = opaque;
    if (level) {
        reset_registers(s);
    }
    s->reset_asserted = !!level;
    irq_update(s);
    if (!level) {
        esp32s3_uhci_kick(s);
    }
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const Property properties[] = {
    DEFINE_PROP_LINK("gdma", ESP32S3UhciState, gdma, TYPE_ESP_GDMA, ESPGdmaState *),
    DEFINE_PROP_LINK("soc-reset", ESP32S3UhciState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};
static void service_bh(void *opaque)
{
    esp32s3_uhci_service(opaque);
}

static void gdma_notify(ESPGdmaState *gdma, GdmaPeripheral periph,
                        uint32_t channel, int dir, void *opaque)
{
    esp32s3_uhci_kick(opaque);
}

static void realize(DeviceState *dev, Error **errp)
{
    ESP32S3UhciState *s = ESP32S3_UHCI(dev);
    if (s->gdma) {
        esp_gdma_set_peripheral_notify(s->gdma, GDMA_UHCI0, gdma_notify, s);
    }
}

static void init(Object *obj)
{
    ESP32S3UhciState *s = ESP32S3_UHCI(obj);
    memory_region_init_io(&s->iomem, obj, &ops, s, TYPE_ESP32S3_UHCI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bh = qemu_bh_new(service_bh, s);
    s->gate = true;
    reset_registers(s);
    qdev_init_gpio_in_named(DEVICE(obj), gate_input, "clk-gate", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_input, "reset", 1);
}
static void finalize(Object *obj)
{
    ESP32S3UhciState *s = ESP32S3_UHCI(obj);
    if (s->gdma) {
        esp_gdma_set_peripheral_notify(s->gdma, GDMA_UHCI0, NULL, NULL);
    }
    qemu_bh_delete(ESP32S3_UHCI(obj)->bh);
}
static void class_init(ObjectClass *klass, void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), properties);
    DEVICE_CLASS(klass)->realize = realize;
    RESETTABLE_CLASS(klass)->phases.hold = reset_hold;
}
static const TypeInfo type = {
    .name = TYPE_ESP32S3_UHCI, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3UhciState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void)
{
    type_register_static(&type);
}
type_init(register_types)
