/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/esp32s3_i2c.h"
#include "hw/irq.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"

#define REG(s, a) ((s)->reg[(a) / 4])
#define RX_WM BIT(0)
#define TX_WM BIT(1)
#define RX_OVF BIT(2)
#define END_DETECT BIT(3)
#define BYTE_DONE BIT(4)
#define ARBITRATION BIT(5)
#define TX_UDF BIT(6)
#define COMPLETE BIT(7)
#define TIMEOUT BIT(8)
#define START_INT BIT(9)
#define NACK_INT BIT(10)
#define TX_OVF BIT(11)
#define RX_UDF BIT(12)
#define CMD_DONE BIT(31)
#define INT_MASK 0x3ffff

static void i2c_irq(ESP32S3I2CState *s)
{
    uint32_t conf = REG(s, 0x18);
    uint32_t raw = REG(s, 0x20) & ~(RX_WM | TX_WM);
    if (conf & BIT(14)) {
        if (fifo8_num_used(&s->rx) > (conf & 31)) {
            raw |= RX_WM;
        }
        if (fifo8_num_used(&s->tx) < ((conf >> 5) & 31)) {
            raw |= TX_WM;
        }
    }
    REG(s, 0x20) = raw;
    qemu_set_irq(s->irq, s->gate && !s->reset_asserted &&
                 (REG(s, 0x54) & BIT(21)) && !!(raw & REG(s, 0x28)));
}

static void drive(ESP32S3I2CState *s, bool sda, bool scl)
{
    s->sda = sda;
    s->scl = scl;
    if (s->provider.drive) {
        s->provider.drive(s->provider.opaque, s->controller, sda, scl);
    }
}

static void release_slave(ESP32S3I2CState *s)
{
    if (s->service && s->provider.slave_drive) {
        s->provider.slave_drive(s->provider.opaque, s->service, true, true);
    }
}

static void cancel(ESP32S3I2CState *s)
{
    S3I2CService *service = s->service;
    s->generation++;
    timer_del(s->timer);
    s->service = NULL;
    s->executing = s->protocol_open = s->waiting = false;
    s->need_address = true;
    s->bit_phase = 0;
    if (service) {
        s3_i2c_service_cancel(service);
        if (s->provider.slave_drive) {
            s->provider.slave_drive(s->provider.opaque, service, true, true);
        }
    }
    drive(s, true, true);
}

static uint64_t source_hz(ESP32S3I2CState *s)
{
    if (!s->gate || s->reset_asserted || !(REG(s, 0x54) & BIT(21))) {
        return 0;
    }
    return clock_get_hz((REG(s, 0x54) & BIT(20)) ? s->rc_fast : s->xtal);
}

static int64_t cycles_ns(ESP32S3I2CState *s, uint64_t cycles)
{
    uint64_t hz = source_hz(s);
    uint32_t div = REG(s, 0x54);
    uint64_t numerator = (div & 255) + 1;
    uint64_t a = (div >> 8) & 63, b = (div >> 14) & 63;
    /* Fraction is DIV_A / DIV_B in the pinned S3 register contract. */
    uint64_t denominator = b ? b : 1;
    numerator = numerator * denominator + (b ? a : 0);
    if (!hz) {
        return 0;
    }
    return MAX(1, muldiv64(cycles * numerator, 1000000000ULL,
                         hz * denominator));
}

static int64_t bit_ns(ESP32S3I2CState *s)
{
    uint32_t high = REG(s, 0x38);
    uint64_t cycles = (REG(s, 0) & 511) + 1 + (high & 511) +
                      ((high >> 9) & 127);
    return cycles_ns(s, cycles);
}

static void schedule(ESP32S3I2CState *s, int64_t delay)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MAX(1, delay));
}

static void fail(ESP32S3I2CState *s, uint32_t irq)
{
    cancel(s);
    fifo8_reset(&s->rx);
    REG(s, 0x20) |= irq;
    i2c_irq(s);
}

static bool released_lines(ESP32S3I2CState *s, int64_t now)
{
    bool sda = false, scl = false;
    uint64_t generation = s->generation;
    bool valid = s->provider.sample &&
                 s->provider.sample(s->provider.opaque, s->controller, &sda, &scl);
    if (s->generation != generation) {
        return false;
    }
    if (valid && sda && scl) {
        s->waiting = false;
        return true;
    }
    if (!s->waiting) {
        s->waiting = true;
        s->wait_start = now;
    }
    uint64_t cycles = 1ULL << (REG(s, 0x0c) & 31);
    int64_t timeout = cycles_ns(s, cycles);
    if ((REG(s, 0x0c) & BIT(5)) && now - s->wait_start >= timeout) {
        fail(s, TIMEOUT);
    } else {
        schedule(s, bit_ns(s));
    }
    return false;
}

static void advance_command(ESP32S3I2CState *s)
{
    REG(s, 0x58 + s->command_index * 4) |= CMD_DONE;
    s->command_index++;
    s->remaining = 0;
    s->bit_phase = 0;
}

/* Timed transaction fast path: every byte samples the same resolved pads and
 * endpoint graph as routing, never a controller/address response cache. This
 * path is not an edge/fast-equivalence or glitch-filter qualification. */
static void step(void *opaque)
{
    ESP32S3I2CState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t generation = s->generation;
    if (!s->executing || !source_hz(s)) {
        return;
    }
    if (s->command_index >= 8) {
        fail(s, TIMEOUT);
        return;
    }
    uint32_t cmd = s->active_cmd[s->command_index];
    unsigned opcode = (cmd >> 11) & 7;
    if (s->bit_phase == 1) {
        drive(s, true, true);
        if (s->generation != generation) {
            return;
        }
        if (!released_lines(s, now)) {
            return;
        }
        s->bit_phase = 2;
    }
    if (opcode == 6) {
        if (!released_lines(s, now)) {
            return;
        }
        s->restart = s->protocol_open;
        s->need_address = true;
        s->protocol_open = true;
        drive(s, false, true);
        if (s->generation != generation) {
            return;
        }
        advance_command(s);
        drive(s, true, true);
        if (s->generation != generation) {
            return;
        }
        schedule(s, cycles_ns(s, (REG(s, 0x40) & 511) + 1 +
                                      (REG(s, 0x44) & 511)));
        return;
    }
    if (opcode == 2 || opcode == 4) {
        if (!released_lines(s, now)) {
            return;
        }
        if (opcode == 2 && !s->bit_phase) {
            drive(s, false, false);
            if (s->generation != generation) {
                return;
            }
            s->bit_phase = 1;
            schedule(s, cycles_ns(s, (REG(s, 0x48) & 511) +
                                      (REG(s, 0x4c) & 511)));
            return;
        }
        advance_command(s);
        s->executing = false;
        if (opcode == 2) {
            if (s->service) {
                s3_i2c_service_stop(s->service, now);
            }
            release_slave(s);
            if (s->generation != generation) {
                return;
            }
            s->service = NULL;
            s->protocol_open = false;
            s->need_address = true;
            REG(s, 0x20) |= COMPLETE;
        } else {
            REG(s, 0x20) |= END_DETECT;
        }
        i2c_irq(s);
        return;
    }
    if (opcode != 1 && opcode != 3) {
        fail(s, TIMEOUT);
        return;
    }
    if (!s->remaining) {
        s->remaining = cmd & 255;
        if (!s->remaining) {
            advance_command(s);
            schedule(s, 1);
            return;
        }
    }
    if (!s->bit_phase) {
        if (!released_lines(s, now)) {
            return;
        }
        if (opcode == 1 && fifo8_is_empty(&s->tx)) {
            fail(s, TX_UDF);
            return;
        }
        if (opcode == 3 && fifo8_is_full(&s->rx)) {
            fail(s, RX_OVF);
            return;
        }
        /* Hold-master conversion stretches this byte in virtual time. */
        if (opcode == 3 && s->service) {
            int64_t ready = s3_i2c_service_ready_ns(s->service);
            if (ready > now) {
                uint64_t limit = 1ULL << (REG(s, 0x0c) & 31);
                int64_t timeout = cycles_ns(s, limit);
                if ((REG(s, 0x0c) & BIT(5)) && ready - now > timeout) {
                    s->waiting = true;
                    s->wait_start = now;
                    s->bit_phase = 3;
                    schedule(s, timeout);
                } else {
                    s->bit_phase = 4;
                    schedule(s, ready - now);
                }
                if (s->provider.slave_drive) {
                    s->provider.slave_drive(s->provider.opaque, s->service, true, false);
                }
                return;
            }
        }
        drive(s, true, false);
        if (s->generation != generation) {
            return;
        }
        s->bit_phase = 1;
        schedule(s, bit_ns(s) * 9);
        return;
    }
    if (s->bit_phase == 3) {
        fail(s, TIMEOUT);
        return;
    }
    if (s->bit_phase == 4) {
        release_slave(s);
        if (s->generation != generation) {
            return;
        }
        s->bit_phase = 0;
        schedule(s, 1);
        return;
    }
    bool ack = false;
    if (!s->need_address && s->service) {
        bool collision = false;
        S3I2CService *reachable = s->provider.resolve ?
            s->provider.resolve(s->provider.opaque, s->controller, s->address,
                                &collision) : NULL;
        if (s->generation != generation) {
            return;
        }
        if (collision || reachable != s->service) {
            fail(s, collision ? ARBITRATION : NACK_INT);
            return;
        }
    }
    if (opcode == 1) {
        s->byte = fifo8_pop(&s->tx);
        if (REG(s, 4) & BIT(6)) {
            s->byte = revbit8(s->byte);
        }
        if (s->need_address) {
            bool collision = false;
            release_slave(s);
            if (s->generation != generation) {
                return;
            }
            s->address = s->byte >> 1;
            S3I2CService *candidate = s->provider.resolve ?
                s->provider.resolve(s->provider.opaque, s->controller, s->byte >> 1,
                                    &collision) : NULL;
            if (s->generation != generation) {
                return;
            }
            s->service = candidate;
            if (collision) {
                fail(s, ARBITRATION);
                return;
            }
            s->reading = s->byte & 1;
            ack = s->service && s3_i2c_service_address(s->service, s->reading,
                                                       s->restart, now);
            s->need_address = false;
        } else {
            ack = s->service && !s->reading &&
                  s3_i2c_service_write(s->service, s->byte, now);
        }
        s->nack = !ack;
        if ((cmd & BIT(8)) && s->nack != !!(cmd & BIT(9))) {
            fail(s, NACK_INT);
            return;
        }
    } else {
        if (!s->service || !s->reading ||
            !s3_i2c_service_read(s->service, &s->byte, now)) {
            fail(s, NACK_INT);
            return;
        }
        fifo8_push(&s->rx, (REG(s, 4) & BIT(7)) ? revbit8(s->byte) : s->byte);
        s3_i2c_service_read_ack(s->service, !!(cmd & BIT(10)));
    }
    REG(s, 0x20) |= BYTE_DONE;
    if (!--s->remaining) {
        advance_command(s);
    } else {
        s->bit_phase = 0;
    }
    i2c_irq(s);
    schedule(s, 1);
}

static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3I2CState *s = opaque;
    if (addr == 8) {
        unsigned main_state = !s->executing ? 0 : s->need_address ? 1 :
                              s->reading ? 3 : 4;
        unsigned scl_state = !s->executing ? 0 : s->scl ? 5 : 3;
        return s->nack | (!!(REG(s, 0x20) & ARBITRATION) << 3) |
               (s->executing << 4) | (fifo8_num_used(&s->rx) << 8) |
               (3 << 14) | (fifo8_num_used(&s->tx) << 18) |
               (main_state << 24) | (scl_state << 28);
    }
    if (addr == 0x14) {
        return s->rx.head | (((s->rx.head + s->rx.num) & 31) << 5) |
               (s->tx.head << 10) | (((s->tx.head + s->tx.num) & 31) << 15);
    }
    if (addr == 0x1c) {
        if (fifo8_is_empty(&s->rx)) {
            REG(s, 0x20) |= RX_UDF;
            i2c_irq(s);
            return 0;
        }
        uint8_t value = fifo8_pop(&s->rx);
        i2c_irq(s);
        return value;
    }
    if (addr == 0x20 || addr == 0x2c) {
        i2c_irq(s);
        return addr == 0x20 ? REG(s, 0x20) : REG(s, 0x20) & REG(s, 0x28);
    }
    if (addr == 0x24) {
        return 0;
    }
    return REG(s, addr);
}

static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3I2CState *s = opaque;
    if (s->reset_asserted) {
        return;
    }
    switch (addr) {
    case 8: case 0x14: case 0x2c:
        return;
    case 0x20: case 0x24:
        REG(s, 0x20) &= ~(value & INT_MASK);
        break;
    case 0x28:
        REG(s, addr) = value & INT_MASK;
        break;
    case 0x1c:
        if (fifo8_is_full(&s->tx)) {
            REG(s, 0x20) |= TX_OVF;
        } else {
            fifo8_push(&s->tx, value);
        }
        break;
    case 0x18:
        if (value & BIT(12)) {
            fifo8_reset(&s->rx);
        }
        if (value & BIT(13)) {
            fifo8_reset(&s->tx);
        }
        REG(s, addr) = value & ~(BIT(12) | BIT(13));
        break;
    case 4:
        if (value & BIT(10)) {
            cancel(s);
        }
        REG(s, addr) = value & ~(BIT(5) | BIT(10) | BIT(11));
        drive(s, s->sda, s->scl);
        if ((value & BIT(5)) && !s->executing && (value & BIT(4)) && source_hz(s)) {
            memcpy(s->active_cmd, &s->reg[0x58 / 4], sizeof(s->active_cmd));
            for (unsigned i = 0; i < 8; i++) {
                REG(s, 0x58 + i * 4) &= ~CMD_DONE;
            }
            s->executing = true;
            s->command_index = s->remaining = s->bit_phase = 0;
            REG(s, 0x20) |= START_INT;
            schedule(s, 1);
        }
        break;
    case 0x54:
        value &= 0x3fffff;
        if (REG(s, addr) != value) {
            cancel(s);
            REG(s, 0x20) = 0;
            REG(s, 0x28) = 0;
        }
        REG(s, addr) = value & 0x3fffff;
        drive(s, s->sda, s->scl);
        break;
    default:
        REG(s, addr) = value;
        break;
    }
    i2c_irq(s);
}

static void reset_registers(ESP32S3I2CState *s)
{
    cancel(s);
    fifo8_reset(&s->tx);
    fifo8_reset(&s->rx);
    memset(s->reg, 0, sizeof(s->reg));
    REG(s, 4) = 3;
    REG(s, 0x0c) = 16;
    REG(s, 0x18) = 11 | (4 << 5) | BIT(14);
    REG(s, 0x40) = REG(s, 0x44) = REG(s, 0x48) = REG(s, 0x4c) = 8;
    REG(s, 0x54) = BIT(21);
    REG(s, 0x78) = REG(s, 0x7c) = 16;
    REG(s, 0xf8) = 537330177;
    s->need_address = true;
    s->nack = false;
    drive(s, true, true);
    i2c_irq(s);
}

static void reset_hold(Object *obj, ResetType type)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);
    if (!s->soc_reset || esp32s3_reset_covers_periph(s->soc_reset)) {
        reset_registers(s);
    }
}

static void gate_input(void *opaque, int n, int level)
{
    ESP32S3I2CState *s = opaque;
    s->gate = !!level;
    if (!level) {
        cancel(s);
        REG(s, 0x20) = 0;
        i2c_irq(s);
    } else {
        drive(s, s->sda, s->scl);
        i2c_irq(s);
    }
}

static void reset_input(void *opaque, int n, int level)
{
    ESP32S3I2CState *s = opaque;
    if (level) {
        reset_registers(s);
    }
    s->reset_asserted = !!level;
    drive(s, s->sda, s->scl);
}

static void clock_changed(ESP32S3I2CState *s)
{
    if (s->executing || s->protocol_open) {
        cancel(s);
        REG(s, 0x20) = 0;
        REG(s, 0x28) = 0;
        i2c_irq(s);
    } else {
        drive(s, s->sda, s->scl);
    }
}

static void xtal_changed(void *opaque, ClockEvent event)
{
    ESP32S3I2CState *s = opaque;
    if (!(REG(s, 0x54) & BIT(20))) {
        clock_changed(s);
    }
}

static void rc_fast_changed(void *opaque, ClockEvent event)
{
    ESP32S3I2CState *s = opaque;
    if (REG(s, 0x54) & BIT(20)) {
        clock_changed(s);
    }
}

void esp32s3_i2c_invalidate(ESP32S3I2CState *s)
{
    fail(s, NACK_INT);
}

void esp32s3_i2c_bind(ESP32S3I2CState *s, const S3I2CProvider *provider)
{
    cancel(s);
    s->provider = *provider;
    drive(s, true, true);
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const Property properties[] = {
    DEFINE_PROP_UINT32("controller", ESP32S3I2CState, controller, 0),
    DEFINE_PROP_LINK("soc-reset", ESP32S3I2CState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void init(Object *obj)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);
    memory_region_init_io(&s->iomem, obj, &ops, s, TYPE_ESP32S3_I2C, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    fifo8_create(&s->tx, 32);
    fifo8_create(&s->rx, 32);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, step, s);
    s->xtal = qdev_init_clock_in(DEVICE(obj), "xtal-clk", xtal_changed, s, ClockUpdate);
    s->rc_fast = qdev_init_clock_in(DEVICE(obj), "rc-fast-clk", rc_fast_changed, s, ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(obj), gate_input, "clk-gate", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_input, "reset", 1);
}

static void finalize(Object *obj)
{
    ESP32S3I2CState *s = ESP32S3_I2C(obj);
    timer_free(s->timer);
    fifo8_destroy(&s->rx);
    fifo8_destroy(&s->tx);
}

static void class_init(ObjectClass *klass, void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), properties);
    RESETTABLE_CLASS(klass)->phases.hold = reset_hold;
}

static const TypeInfo type = {
    .name = TYPE_ESP32S3_I2C, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3I2CState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void)
{
    type_register_static(&type);
}
type_init(register_types)
