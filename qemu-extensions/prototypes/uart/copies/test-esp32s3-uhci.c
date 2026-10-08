/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native controller unit vectors. Link the real S3 GDMA + UHCI device objects.
 * Endpoint FIFO stand-ins below test only the byte attach boundary; they do not
 * model GPIO/UART frames and are not used by the runtime or firmware fixtures.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/char/esp32s3_uhci.h"
#include "hw/dma/esp32s3_gdma.h"
#include "hw/qdev-properties.h"
#include "sysemu/sysemu.h"

#define BASE 0x3fc80000u
#define DESC 0x3fc88000u
#define BUFFER 0x3fc89000u
#define OWNER (1u << 31)
#define EOF_BIT (1u << 30)

typedef struct Endpoint {
    uint8_t rx[2048], tx[2048];
    unsigned rx_head, rx_tail, tx_len, capacity;
} Endpoint;
typedef struct Fixture {
    MemoryRegion root, ram;
    ESPGdmaState *gdma;
    ESP32S3UhciState *uhci;
    Endpoint uart[3];
    uint8_t *bytes;
} Fixture;

static bool endpoint_write(void *opaque, uint8_t byte)
{
    Endpoint *u = opaque;
    if (u->tx_len == u->capacity) {
        return false;
    }
    u->tx[u->tx_len++] = byte;
    return true;
}
static bool endpoint_read(void *opaque, uint8_t *byte)
{
    Endpoint *u = opaque;
    if (u->rx_head == u->rx_tail) {
        return false;
    }
    *byte = u->rx[u->rx_head++];
    return true;
}
static unsigned endpoint_free(void *opaque)
{
    Endpoint *u = opaque;
    return u->capacity - u->tx_len;
}
static unsigned endpoint_used(void *opaque)
{
    Endpoint *u = opaque;
    return u->rx_tail - u->rx_head;
}
static void configure(Fixture *f, unsigned offset, uint32_t value)
{
    g_assert_cmpint(memory_region_dispatch_write(&f->uhci->iomem, offset, value,
                   MO_32, MEMTXATTRS_UNSPECIFIED), ==, MEMTX_OK);
}
static void descriptor(Fixture *f, unsigned slot, uint32_t dw0, uint32_t next)
{
    uint8_t *p = f->bytes + DESC - BASE + slot * 16;
    stl_le_p(p, dw0);
    stl_le_p(p + 4, BUFFER + slot * 256);
    stl_le_p(p + 8, next);
}
static void arm(Fixture *f, int dir, unsigned chan, unsigned slot,
                GdmaPeripheral peripheral)
{
    esp_gdma_write_chan_register(f->gdma, dir, chan, GDMA_CONF1_REG, 1u << 12);
    if (dir == ESP_GDMA_OUT_IDX) {
        esp_gdma_write_chan_register(f->gdma, dir, chan, GDMA_CONF0_REG, 1u << 2);
    }
    esp_gdma_write_chan_register(f->gdma, dir, chan, GDMA_PERI_SEL_REG, peripheral);
    esp_gdma_write_chan_register(f->gdma, dir, chan, GDMA_LINK_REG,
        ((DESC + slot * 16) & 0xfffff) | (1u << (dir == ESP_GDMA_IN_IDX ? 22 : 21)));
}
static void setup(Fixture *f, const void *data)
{
    memory_region_init(&f->root, NULL, "uhci-test-root", UINT64_MAX);
    memory_region_init_ram(&f->ram, NULL, "uhci-test-ram", 0x80000, &error_abort);
    memory_region_add_subregion(&f->root, BASE, &f->ram);
    f->bytes = memory_region_get_ram_ptr(&f->ram);
    f->gdma = ESP_GDMA(qdev_new(TYPE_ESP32S3_GDMA));
    object_property_set_link(OBJECT(f->gdma), "soc_mr", OBJECT(&f->root), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(f->gdma), &error_abort);
    f->uhci = ESP32S3_UHCI(qdev_new(TYPE_ESP32S3_UHCI));
    object_property_set_link(OBJECT(f->uhci), "gdma", OBJECT(f->gdma), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(f->uhci), &error_abort);
    for (unsigned i = 0; i < 3; i++) {
        f->uart[i].capacity = sizeof(f->uart[i].tx);
        S3UhciUart ops = { &f->uart[i], endpoint_write, endpoint_read,
                          endpoint_free, endpoint_used };
        esp32s3_uhci_attach_uart(f->uhci, i, &ops);
    }
    configure(f, 0, 1u << 3); /* raw mode, UART1 */
    configure(f, 0x24, 0);
}
static void teardown(Fixture *f, const void *data)
{
    qdev_unrealize(DEVICE(f->uhci));
    object_unparent(OBJECT(f->uhci));
    object_unref(OBJECT(f->uhci));
    qdev_unrealize(DEVICE(f->gdma));
    address_space_destroy(&f->gdma->dma_as);
    object_unparent(OBJECT(f->gdma));
    object_unref(OBJECT(f->gdma));
    memory_region_del_subregion(&f->root, &f->ram);
    /* Owner-less memory regions are QOM-attached under /machine/unattached;
     * detaching also releases the reference held on their behalf. */
    object_unparent(OBJECT(&f->ram));
    object_unparent(OBJECT(&f->root));
}

static void full_duplex(Fixture *f, const void *data)
{
    const uint8_t tx[] = { 0x10, 0x22, 0x37 };
    const uint8_t rx[] = { 0xa5, 0x5a };
    descriptor(f, 0, OWNER | EOF_BIT | (sizeof(tx) << 12) | sizeof(tx), 0);
    descriptor(f, 1, OWNER | 64, 0);
    memcpy(f->bytes + BUFFER - BASE, tx, sizeof(tx));
    memcpy(f->uart[1].rx, rx, sizeof(rx));
    f->uart[1].rx_tail = sizeof(rx);
    arm(f, ESP_GDMA_OUT_IDX, 3, 0, GDMA_UHCI0);
    arm(f, ESP_GDMA_IN_IDX, 1, 1, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_IDLE, false,
                         endpoint_used(&f->uart[1])); /* disabled: no EOF */
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE + 16) & OWNER, !=, 0);
    configure(f, 0, (1u << 3) | (1u << 8));
    esp32s3_uhci_rx_event(f->uhci, 0, S3_UHCI_RX_IDLE, false,
                         endpoint_used(&f->uart[0])); /* wrong UART ignored */
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE + 16) & OWNER, !=, 0);
    esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_IDLE, false,
                         endpoint_used(&f->uart[1]));
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpmem(f->uart[1].tx, f->uart[1].tx_len, tx, sizeof(tx));
    g_assert_cmpmem(f->bytes + BUFFER - BASE + 256, sizeof(rx), rx, sizeof(rx));
    uint32_t dw0 = ldl_le_p(f->bytes + DESC - BASE + 16);
    g_assert_cmpuint(dw0 & OWNER, ==, 0);
    g_assert_cmpuint(dw0 & EOF_BIT, !=, 0);
    g_assert_cmpuint((dw0 >> 12) & 0xfff, ==, sizeof(rx));
    g_assert_cmpuint(f->gdma->ch_conf[ESP_GDMA_IN_IDX][1].suc_eof_desc_addr, ==, DESC + 16);
    g_assert_cmpuint(f->uart[0].tx_len + f->uart[2].tx_len, ==, 0);
}
static void ownership_and_isolation(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | EOF_BIT | (1u << 12) | 1, 0);
    descriptor(f, 1, EOF_BIT | (1u << 12) | 1, 0);
    f->bytes[BUFFER - BASE] = 0x44;
    arm(f, ESP_GDMA_OUT_IDX, 0, 0, GDMA_SPI2);
    arm(f, ESP_GDMA_OUT_IDX, 1, 1, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->uart[1].tx_len, ==, 0);
    g_assert_true(f->gdma->ch_conf[ESP_GDMA_OUT_IDX][1].halted);
    g_assert_true(f->gdma->ch_conf[ESP_GDMA_OUT_IDX][0].fsm_active);
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE) & OWNER, !=, 0);
    arm(f, ESP_GDMA_OUT_IDX, 2, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->uart[1].tx_len, ==, 1);
    g_assert_cmpuint(f->uart[1].tx[0], ==, 0x44);
}
static void framed_escapes(Fixture *f, const void *data)
{
    const uint8_t payload[] = { 0xc0, 0xdb, 0x11, 0x13, 0x55 };
    const uint8_t wire[] = { 0xc0, 0xdb, 0xdc, 0xdb, 0xdd, 0xdb, 0xde,
                             0xdb, 0xdf, 0x55, 0xc0 };
    descriptor(f, 0, OWNER | EOF_BIT | (sizeof(payload) << 12) | sizeof(payload), 0);
    descriptor(f, 1, OWNER | 64, 0);
    memcpy(f->bytes + BUFFER - BASE, payload, sizeof(payload));
    memcpy(f->uart[1].rx, wire, sizeof(wire));
    f->uart[1].rx_tail = sizeof(wire);
    configure(f, 0, (1u << 3) | (1u << 5));
    configure(f, 0x24, 0xff);
    arm(f, ESP_GDMA_OUT_IDX, 4, 0, GDMA_UHCI0);
    arm(f, ESP_GDMA_IN_IDX, 0, 1, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpmem(f->uart[1].tx, f->uart[1].tx_len, wire, sizeof(wire));
    g_assert_cmpmem(f->bytes + BUFFER - BASE + 256, sizeof(payload), payload, sizeof(payload));
    g_assert_cmpuint((ldl_le_p(f->bytes + DESC - BASE + 16) >> 12) & 0xfff,
                     ==, sizeof(payload));
    g_assert_cmpuint(f->uhci->reg[1] & 3, ==, 3);
}
static void backpressure_selection(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | EOF_BIT | (3u << 12) | 3, 0);
    memcpy(f->bytes + BUFFER - BASE, "abc", 3);
    f->uart[1].capacity = 1;
    arm(f, ESP_GDMA_OUT_IDX, 0, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->uart[1].tx_len, ==, 1);
    configure(f, 0, 1u << 4); /* in-flight change cannot leak into UART2 */
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->uart[2].tx_len, ==, 0);
    configure(f, 0, (1u << 3) | (1u << 4)); /* ambiguous selection suspended */
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->uart[2].tx_len, ==, 0);
    configure(f, 0, 1u << 3);
    f->uart[1].capacity = 128;
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpmem(f->uart[1].tx, f->uart[1].tx_len, "abc", 3);
}
static void malformed_receive(Fixture *f, const void *data)
{
    const uint8_t wire[] = { 0xc0, 0x41, 0xdb, 0x01 };
    descriptor(f, 0, OWNER | 64, 0);
    configure(f, 0, (1u << 3) | (1u << 5));
    configure(f, 0x24, 0x30);
    memcpy(f->uart[1].rx, wire, sizeof(wire));
    f->uart[1].rx_tail = sizeof(wire);
    arm(f, ESP_GDMA_IN_IDX, 2, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->bytes[BUFFER - BASE], ==, 0x41);
    g_assert_cmpuint((ldl_le_p(f->bytes + DESC - BASE) >> 12) & 0xfff, ==, 1);
    g_assert_cmpuint(f->gdma->ch_conf[ESP_GDMA_IN_IDX][2].err_eof_desc_addr, ==, DESC);
    g_assert_cmpuint(f->gdma->ch_conf[ESP_GDMA_IN_IDX][2].int_state.raw & 7, ==, 7);
}
static void threshold_exact_boundary(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | 2, DESC + 16);
    descriptor(f, 1, OWNER | 64, 0);
    f->uart[1].rx[0] = 0x77;
    f->uart[1].rx[1] = 0x88;
    f->uart[1].rx_tail = 2;
    configure(f, 0, (1u << 3) | (1u << 9));
    configure(f, 0x80, 2);
    arm(f, ESP_GDMA_IN_IDX, 3, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE) & EOF_BIT, !=, 0);
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE + 16) & OWNER, !=, 0);
    g_assert_cmpuint(f->gdma->ch_conf[ESP_GDMA_IN_IDX][3].suc_eof_desc_addr, ==, DESC);
}

static void accepted_byte_then_fault(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | (1u << 12) | 1, DESC + 16);
    descriptor(f, 1, (1u << 12) | 1, 0); /* next descriptor belongs to CPU */
    f->bytes[BUFFER - BASE] = 0x91;
    arm(f, ESP_GDMA_OUT_IDX, 0, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->uart[1].tx_len, ==, 1);
    g_assert_cmpuint(f->uart[1].tx[0], ==, 0x91);
    g_assert_true(f->gdma->ch_conf[ESP_GDMA_OUT_IDX][0].halted);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->uart[1].tx_len, ==, 1);
    descriptor(f, 2, OWNER | 1, DESC + 48);
    descriptor(f, 3, 64, 0); /* next descriptor belongs to CPU */
    f->uart[1].rx[0] = 0x82;
    f->uart[1].rx_tail = 1;
    arm(f, ESP_GDMA_IN_IDX, 4, 2, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpuint(f->bytes[BUFFER - BASE + 512], ==, 0x82);
    g_assert_false(f->uhci->rx_have_pending);
    g_assert_true(f->gdma->ch_conf[ESP_GDMA_IN_IDX][4].halted);
}

static void receive_wrong_owner(Fixture *f, const void *data)
{
    descriptor(f, 0, 64, 0);
    f->uart[1].rx[0] = 0x12;
    f->uart[1].rx_tail = 1;
    arm(f, ESP_GDMA_IN_IDX, 1, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_true(f->gdma->ch_conf[ESP_GDMA_IN_IDX][1].halted);
    g_assert_cmpuint(f->uart[1].rx_head, ==, 0);
    g_assert_cmpuint(f->bytes[BUFFER - BASE], ==, 0);
}

static void pending_idle_drains_fifo(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | 64, 0);
    configure(f, 0, (1u << 3) | (1u << 8));
    memcpy(f->uart[1].rx, "last", 4);
    f->uart[1].rx_tail = 4;
    arm(f, ESP_GDMA_IN_IDX, 4, 0, GDMA_UHCI0);
    esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_IDLE, false,
                         endpoint_used(&f->uart[1]));
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpmem(f->bytes + BUFFER - BASE, 4, "last", 4);
    g_assert_cmpuint((ldl_le_p(f->bytes + DESC - BASE) >> 12) & 0xfff, ==, 4);
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE) & OWNER, ==, 0);
}

static void malformed_cannot_finish_spi(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | 64, 0);
    arm(f, ESP_GDMA_IN_IDX, 1, 0, GDMA_SPI2);
    uint8_t byte = 0x65;
    g_assert_true(esp_gdma_write_channel(f->gdma, 1, &byte, 1));
    g_assert_false(esp_gdma_finish_rx_channel_err(f->gdma, 1));
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE) & OWNER, !=, 0);
    g_assert_true(f->gdma->ch_conf[ESP_GDMA_IN_IDX][1].fsm_active);
    g_assert_cmpuint(f->gdma->ch_conf[ESP_GDMA_IN_IDX][1].int_state.raw &
                    (1u << 2), ==, 0);
}

typedef struct Notification {
    unsigned calls, channel;
    int direction;
} Notification;

static void record_notify(ESPGdmaState *gdma, GdmaPeripheral periph,
                           uint32_t chan, int dir, void *opaque)
{
    Notification *n = opaque;
    g_assert_cmpint(periph, ==, GDMA_UHCI0);
    n->calls++;
    n->channel = chan;
    n->direction = dir;
}

static void demand_notifications(Fixture *f, const void *data)
{
    Notification n = { 0 };
    esp_gdma_set_peripheral_notify(f->gdma, GDMA_UHCI0, record_notify, &n);
    descriptor(f, 0, OWNER | EOF_BIT | (1u << 12) | 1, 0);
    descriptor(f, 1, EOF_BIT | (1u << 12) | 1, 0);
    arm(f, ESP_GDMA_OUT_IDX, 4, 0, GDMA_UHCI0);
    g_assert_cmpuint(n.calls, ==, 1);
    g_assert_cmpuint(n.channel, ==, 4);
    g_assert_cmpint(n.direction, ==, ESP_GDMA_OUT_IDX);
    arm(f, ESP_GDMA_OUT_IDX, 3, 1, GDMA_UHCI0); /* failed owner check */
    arm(f, ESP_GDMA_OUT_IDX, 2, 0, GDMA_SPI2); /* different peripheral */
    g_assert_cmpuint(n.calls, ==, 1);
    esp_gdma_write_chan_register(f->gdma, ESP_GDMA_OUT_IDX, 4, GDMA_LINK_REG,
                                 1u << 20); /* STOP, retain byte cursor */
    esp_gdma_write_chan_register(f->gdma, ESP_GDMA_OUT_IDX, 4, GDMA_LINK_REG,
                                 1u << 22); /* RESTART */
    g_assert_cmpuint(n.calls, ==, 2);
    descriptor(f, 2, OWNER | 64, 0);
    esp_gdma_write_chan_register(f->gdma, ESP_GDMA_IN_IDX, 0, GDMA_CONF0_REG,
                                 1u << 4); /* memory-to-memory */
    arm(f, ESP_GDMA_OUT_IDX, 0, 0, GDMA_UHCI0);
    arm(f, ESP_GDMA_IN_IDX, 0, 2, GDMA_UHCI0);
    g_assert_cmpuint(n.calls, ==, 2);
    esp_gdma_set_peripheral_notify(f->gdma, GDMA_UHCI0, NULL, NULL);
}

static void segment_partial_continues(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | 64, DESC + 16);
    descriptor(f, 1, OWNER | 64, 0);
    arm(f, ESP_GDMA_IN_IDX, 0, 0, GDMA_I2S0);
    uint8_t first[] = { 0x11, 0x22, 0x33 };
    uint8_t second[] = { 0x44, 0x55 };
    uint32_t served;
    g_assert_true(esp_gdma_write_channel_ex(f->gdma, 0, first, 3, &served));
    g_assert_cmpuint(served, ==, 3);
    esp_gdma_write_chan_register(f->gdma, ESP_GDMA_IN_IDX, 0, GDMA_INT_CLR_REG,
                                 0xffffffff);
    g_assert_true(esp_gdma_finish_rx_segment(f->gdma, 0));
    DmaConfigState *st = &f->gdma->ch_conf[ESP_GDMA_IN_IDX][0];
    uint32_t dw0 = ldl_le_p(f->bytes + DESC - BASE);
    g_assert_cmpuint((dw0 >> 12) & 0xfff, ==, 3);
    g_assert_cmpuint(dw0 & OWNER, ==, 0);
    g_assert_cmpuint(dw0 & EOF_BIT, !=, 0);
    g_assert_cmpuint(st->cur_desc_addr, ==, DESC + 16);
    g_assert_cmpuint(st->cur_buf_off, ==, 0);
    g_assert_true(st->fsm_active);
    g_assert_false(st->rx_packet_ended);
    g_assert_cmpuint(st->suc_eof_desc_addr, ==, DESC);
    g_assert_cmpuint(st->int_state.raw & 3, ==, 2); /* no extra IN_DONE */
    g_assert_false(esp_gdma_finish_rx_segment(f->gdma, 0));
    g_assert_true(esp_gdma_write_channel_ex(f->gdma, 0, second, 2, &served));
    g_assert_true(esp_gdma_finish_rx_segment(f->gdma, 0));
    g_assert_cmpmem(f->bytes + BUFFER - BASE, 3, first, 3);
    g_assert_cmpmem(f->bytes + BUFFER - BASE + 256, 2, second, 2);
    g_assert_false(st->fsm_active); /* actual DW2=0, not original link restart */
    g_assert_cmpuint(st->suc_eof_desc_addr, ==, DESC + 16);
}

static void segment_full_preserves_prefetch(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | 2, DESC + 16);
    descriptor(f, 1, OWNER | 64, 0);
    arm(f, ESP_GDMA_IN_IDX, 2, 0, GDMA_I2S1);
    uint8_t bytes[] = { 0x61, 0x62 };
    g_assert_true(esp_gdma_write_channel(f->gdma, 2, bytes, 2));
    DmaConfigState *st = &f->gdma->ch_conf[ESP_GDMA_IN_IDX][2];
    g_assert_cmpuint(st->cur_desc_addr, ==, DESC + 16);
    g_assert_cmpuint(st->cur_buf_off, ==, 0);
    g_assert_true(esp_gdma_finish_rx_segment(f->gdma, 2));
    g_assert_cmpuint(st->cur_desc_addr, ==, DESC + 16);
    g_assert_cmpuint(st->cur_buf_off, ==, 0);
    g_assert_true(st->fsm_active);
    g_assert_cmpuint(st->suc_eof_desc_addr, ==, DESC);
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE) & EOF_BIT, !=, 0);
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE + 16) & OWNER, !=, 0);
    g_assert_false(esp_gdma_finish_rx_segment(f->gdma, 2));
    g_assert_true(esp_gdma_write_channel(f->gdma, 2, bytes, 2));
    g_assert_true(esp_gdma_finish_rx_channel(f->gdma, 2));
    g_assert_false(esp_gdma_finish_rx_segment(f->gdma, 2)); /* terminal supersedes */
}

static void segment_advance_fault_and_reset(Fixture *f, const void *data)
{
    descriptor(f, 0, OWNER | 64, DESC + 16);
    descriptor(f, 1, 64, 0); /* CPU owner, fault only when segment advances */
    arm(f, ESP_GDMA_IN_IDX, 3, 0, GDMA_I2S0);
    uint8_t byte = 0x71;
    g_assert_true(esp_gdma_write_channel(f->gdma, 3, &byte, 1));
    g_assert_true(esp_gdma_finish_rx_segment(f->gdma, 3)); /* committed */
    DmaConfigState *st = &f->gdma->ch_conf[ESP_GDMA_IN_IDX][3];
    g_assert_true(st->halted);
    g_assert_cmpuint(st->suc_eof_desc_addr, ==, DESC);
    g_assert_cmpuint(st->int_state.raw & ((1u << 3) | (1u << 1)), ==, 10);
    g_assert_false(esp_gdma_finish_rx_segment(f->gdma, 3));
    descriptor(f, 2, OWNER | 64, 0);
    arm(f, ESP_GDMA_IN_IDX, 4, 2, GDMA_I2S1);
    g_assert_true(esp_gdma_write_channel(f->gdma, 4, &byte, 1));
    esp_gdma_write_chan_register(f->gdma, ESP_GDMA_IN_IDX, 4, GDMA_CONF0_REG, 1);
    esp_gdma_write_chan_register(f->gdma, ESP_GDMA_IN_IDX, 4, GDMA_CONF0_REG, 0);
    g_assert_false(esp_gdma_finish_rx_segment(f->gdma, 4));
    g_assert_cmpuint(ldl_le_p(f->bytes + DESC - BASE + 32) & OWNER, !=, 0);
}

static void queued_break_delimiter(Fixture *f, const void *data)
{
    const uint8_t payload[] = { 0x11, 0x00, 0x22 };
    const uint8_t later[] = { 0x33, 0x44 };
    descriptor(f, 0, OWNER | 64, DESC + 16);
    descriptor(f, 1, OWNER | 64, 0);
    configure(f, 0, (1u << 3) | (1u << 12) | (1u << 8));
    memcpy(f->uart[1].rx, payload, sizeof(payload));
    f->uart[1].rx[sizeof(payload)] = 0; /* actual queued all-low frame */
    f->uart[1].rx_tail = sizeof(payload) + 1;
    esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_BREAK, true,
                         endpoint_used(&f->uart[1]));
    memcpy(f->uart[1].rx + f->uart[1].rx_tail, later, sizeof(later));
    f->uart[1].rx_tail += sizeof(later); /* arrives before deferred service */
    arm(f, ESP_GDMA_IN_IDX, 3, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpmem(f->bytes + BUFFER - BASE, sizeof(payload), payload, sizeof(payload));
    g_assert_cmpuint((ldl_le_p(f->bytes + DESC - BASE) >> 12) & 0xfff,
                     ==, sizeof(payload));
    g_assert_cmpuint(f->uart[1].rx_head, ==, sizeof(payload) + 1);
    g_assert_cmpuint(endpoint_used(&f->uart[1]), ==, sizeof(later));
    esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_IDLE, false,
                         endpoint_used(&f->uart[1]));
    arm(f, ESP_GDMA_IN_IDX, 3, 1, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpmem(f->bytes + BUFFER - BASE + 256, sizeof(later), later, sizeof(later));
}

static void masked_break_preserves_zero_payload(Fixture *f, const void *data)
{
    const uint8_t payload[] = { 0x17, 0x00, 0x28 };
    descriptor(f, 0, OWNER | 64, 0);
    configure(f, 0, (1u << 3) | (1u << 12));
    memcpy(f->uart[1].rx, payload, sizeof(payload));
    f->uart[1].rx_tail = sizeof(payload);
    /* UART actually did not enqueue the error frame (mask/overflow); no NULL
     * is manufactured and zero-valued payload bytes remain ordinary data. */
    esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_BREAK, false,
                         endpoint_used(&f->uart[1]));
    f->uart[1].rx[f->uart[1].rx_tail++] = 0;
    arm(f, ESP_GDMA_IN_IDX, 2, 0, GDMA_UHCI0);
    esp32s3_uhci_service(f->uhci);
    g_assert_cmpmem(f->bytes + BUFFER - BASE, sizeof(payload), payload, sizeof(payload));
    g_assert_cmpuint((ldl_le_p(f->bytes + DESC - BASE) >> 12) & 0xfff,
                     ==, sizeof(payload));
    g_assert_cmpuint(endpoint_used(&f->uart[1]), ==, 1);
    g_assert_cmpuint(f->uart[1].rx[f->uart[1].rx_head], ==, 0);
}

static void many_exact_idle_boundaries(Fixture *f, const void *data)
{
    configure(f, 0, (1u << 3) | (1u << 8));
    for (unsigned i = 0; i < 40; i++) {
        descriptor(f, i, OWNER | 64, 0);
        f->uart[1].rx[f->uart[1].rx_tail++] = 0x40 + i;
        esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_IDLE, false,
                             endpoint_used(&f->uart[1]));
        /* Repeated idle notifications at the same position must not allocate
         * another boundary or fabricate an empty packet. */
        esp32s3_uhci_rx_event(f->uhci, 1, S3_UHCI_RX_IDLE, false,
                             endpoint_used(&f->uart[1]));
    }
    g_assert_cmpuint(f->uhci->rx_event_count, ==, 40);
    for (unsigned i = 0; i < 40; i++) {
        arm(f, ESP_GDMA_IN_IDX, 1, i, GDMA_UHCI0);
        esp32s3_uhci_service(f->uhci);
        g_assert_cmpuint(f->bytes[BUFFER - BASE + i * 256], ==, 0x40 + i);
        g_assert_cmpuint((ldl_le_p(f->bytes + DESC - BASE + i * 16) >> 12) &
                        0xfff, ==, 1);
        g_assert_cmpuint(f->uart[1].rx_head, ==, i + 1);
        g_assert_cmpuint(f->uhci->rx_event_count, ==, 39 - i);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* Use the same real system initialization as the emulator, but no CPUs,
     * physical machine peripherals or console. Native devices below remain
     * isolated objects with their own RAM/address space. */
    char *qemu_argv[] = { argv[0], "-machine", "none", "-accel", "qtest",
                         "-display", "none", "-nodefaults", "-S", NULL };
    qemu_init(ARRAY_SIZE(qemu_argv) - 1, qemu_argv);
#define VECTOR(name, fn) g_test_add("/esp32s3/uhci/" name, Fixture, NULL, setup, fn, teardown)
    VECTOR("full-duplex", full_duplex);
    VECTOR("owner-peripheral-isolation", ownership_and_isolation);
    VECTOR("framed-four-escapes", framed_escapes);
    VECTOR("backpressure-selection", backpressure_selection);
    VECTOR("malformed-rx", malformed_receive);
    VECTOR("threshold-exact-boundary", threshold_exact_boundary);
    VECTOR("accepted-byte-next-descriptor-fault", accepted_byte_then_fault);
    VECTOR("rx-wrong-owner", receive_wrong_owner);
    VECTOR("idle-drains-last-fifo-bytes", pending_idle_drains_fifo);
    VECTOR("error-eof-cannot-finish-spi", malformed_cannot_finish_spi);
    VECTOR("demand-arm-restart-notify", demand_notifications);
    VECTOR("segment-partial-continues-next", segment_partial_continues);
    VECTOR("segment-full-preserves-prefetch", segment_full_preserves_prefetch);
    VECTOR("segment-advance-fault-and-reset", segment_advance_fault_and_reset);
    VECTOR("break-qualified-queued-delimiter", queued_break_delimiter);
    VECTOR("break-masked-preserves-zero-payload", masked_break_preserves_zero_payload);
    VECTOR("forty-exact-idle-boundaries", many_exact_idle_boundaries);
    int result = g_test_run();
    qemu_cleanup(result);
    return result;
}
