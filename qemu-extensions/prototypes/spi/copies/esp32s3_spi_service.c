/*
 * Persistent edge-level JEDEC NOR service, explicit 24-bit mode-0/3 profile.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Commands: 9F ID, 03 read, 0B fast read (one dummy byte), 05 status,
 * 06 WREN, 04 WRDI, 02 page program, 20 4-KiB erase, D8 64-KiB erase,
 * C7/60 chip erase. Initially erased; no favorable preloaded RX contents.
 * Status bit 0 is WIP, bit 1 is WEL; other status bits are zero.
 * Busy accepts only 05. Unsupported commands float MISO until the next CS.
 *
 * Program uses a 256-byte latch: addresses wrap within the starting page,
 * and the last supplied byte at a wrapped position wins in that latch.
 * Commit applies memory &= latch, so programming never sets a stored zero.
 * Partial command/address/data bytes invalidate a mutating CS transaction.
 * WREN/WRDI and erase reject trailing input; reads ignore response-phase MOSI.
 * Busy begins at clean CS deassert and storage changes only at its deadline.
 * Power loss before completion discards the entire staged operation; this is
 * a deterministic atomic power-failure profile, not analog cell simulation.
 */
#include "qemu/osdep.h"
#include "hw/ssi/esp32s3_spi_service.h"

static const S3SPINorConfig default_config = {
    .size_bytes = 1024u * 1024u,
    .jedec_id = { 0xef, 0x40, 0x14 },
    .program_ns = 700000,
    .sector_erase_ns = 45000000,
    .block_erase_ns = 150000000,
    .chip_erase_ns = 2000000000,
};

static void reset_transaction(S3SPINor *s)
{
    s->phase = S3_SPI_NOR_COMMAND;
    s->command = 0;
    s->address = 0;
    s->address_bytes = 0;
    s->rx_byte = 0;
    s->rx_bits = 0;
    s->tx_byte = 0;
    s->tx_bits = 0;
    s->id_pos = 0;
    s->program_data = false;
    s->response = false;
    s->miso = false;
    s->driven = false;
    /* The page latch may back an already-started busy operation. */
}

bool s3_spi_nor_init(S3SPINor *s, const S3SPINorConfig *config)
{
    const S3SPINorConfig *c = config ? config : &default_config;

    memset(s, 0, sizeof(*s));
    if (c->size_bytes < 65536u || c->size_bytes > S3_SPI_NOR_MAX_SIZE ||
        (c->size_bytes & (c->size_bytes - 1)) || !c->program_ns ||
        !c->sector_erase_ns || !c->block_erase_ns || !c->chip_erase_ns) {
        return false;
    }
    s->memory = g_try_malloc(c->size_bytes);
    if (!s->memory) {
        return false;
    }
    s->config = *c;
    memset(s->memory, 0xff, c->size_bytes);
    s->powered = true;
    reset_transaction(s);
    return true;
}

void s3_spi_nor_cleanup(S3SPINor *s)
{
    g_free(s->memory);
    memset(s, 0, sizeof(*s));
}

void s3_spi_nor_advance(S3SPINor *s, uint64_t now_ns)
{
    unsigned int i;

    if (!s->powered || s->operation == S3_SPI_NOR_IDLE ||
        now_ns < s->busy_until_ns) {
        return;
    }
    if (s->operation == S3_SPI_NOR_PROGRAM_PENDING) {
        for (i = 0; i < S3_SPI_NOR_PAGE_SIZE; i++) {
            s->memory[s->pending_base + i] &= s->page[i];
        }
    } else {
        memset(s->memory + s->pending_base, 0xff, s->pending_length);
    }
    s->operation = S3_SPI_NOR_IDLE;
    s->busy_until_ns = 0;
}

static uint8_t status(const S3SPINor *s)
{
    return (s->operation != S3_SPI_NOR_IDLE ? 1 : 0) |
           (s->write_enable ? 2 : 0);
}

static void next_response(S3SPINor *s)
{
    switch (s->command) {
    case 0x9f:
        s->tx_byte = s->id_pos < 3 ? s->config.jedec_id[s->id_pos++] : 0xff;
        break;
    case 0x05:
        s->tx_byte = status(s);
        break;
    case 0x03:
    case 0x0b:
        s->tx_byte = s->memory[s->address];
        s->address = (s->address + 1) & (s->config.size_bytes - 1);
        break;
    default:
        g_assert_not_reached();
    }
    s->tx_bits = 0;
}

static void start_response(S3SPINor *s)
{
    s->phase = S3_SPI_NOR_RESPONSE;
    s->response = true;
    next_response(s);
}

static void receive_command(S3SPINor *s, uint8_t byte)
{
    s->command = byte;
    if (s->operation != S3_SPI_NOR_IDLE && byte != 0x05) {
        s->phase = S3_SPI_NOR_INVALID;
        return;
    }
    switch (byte) {
    case 0x9f:
    case 0x05:
        start_response(s);
        break;
    case 0x03:
    case 0x0b:
        s->phase = S3_SPI_NOR_ADDRESS;
        break;
    case 0x02:
    case 0x20:
    case 0xd8:
        s->phase = s->write_enable ? S3_SPI_NOR_ADDRESS : S3_SPI_NOR_INVALID;
        break;
    case 0xc7:
    case 0x60:
        s->phase = s->write_enable ? S3_SPI_NOR_END : S3_SPI_NOR_INVALID;
        break;
    case 0x06:
    case 0x04:
        s->phase = S3_SPI_NOR_END;
        break;
    default:
        s->phase = S3_SPI_NOR_INVALID;
        break;
    }
}

static void receive_byte(S3SPINor *s, uint8_t byte)
{
    switch (s->phase) {
    case S3_SPI_NOR_COMMAND:
        receive_command(s, byte);
        break;
    case S3_SPI_NOR_ADDRESS:
        s->address = (s->address << 8) | byte;
        if (++s->address_bytes != 3) {
            break;
        }
        s->address &= s->config.size_bytes - 1;
        if (s->command == 0x03) {
            start_response(s);
        } else if (s->command == 0x0b) {
            s->phase = S3_SPI_NOR_DUMMY;
        } else if (s->command == 0x02) {
            s->page_base = s->address & ~(S3_SPI_NOR_PAGE_SIZE - 1);
            s->page_pos = s->address & (S3_SPI_NOR_PAGE_SIZE - 1);
            memset(s->page, 0xff, sizeof(s->page));
            s->phase = S3_SPI_NOR_PROGRAM;
        } else {
            s->phase = S3_SPI_NOR_END;
        }
        break;
    case S3_SPI_NOR_DUMMY:
        start_response(s);
        break;
    case S3_SPI_NOR_PROGRAM:
        s->page[s->page_pos] = byte;
        s->page_pos = (s->page_pos + 1) & (S3_SPI_NOR_PAGE_SIZE - 1);
        s->program_data = true;
        break;
    case S3_SPI_NOR_END:
        s->phase = S3_SPI_NOR_INVALID;
        break;
    case S3_SPI_NOR_RESPONSE:
    case S3_SPI_NOR_INVALID:
        break;
    }
}

static void begin_operation(S3SPINor *s, uint64_t now_ns, uint64_t duration)
{
    s->write_enable = false;
    s->busy_until_ns = now_ns > UINT64_MAX - duration ?
                       UINT64_MAX : now_ns + duration;
}

static void finalize(S3SPINor *s, uint64_t now_ns)
{
    uint64_t duration;

    if (s->rx_bits || s->phase == S3_SPI_NOR_INVALID ||
        s->operation != S3_SPI_NOR_IDLE) {
        return;
    }
    if (s->phase == S3_SPI_NOR_END && s->command == 0x06) {
        s->write_enable = true;
        return;
    }
    if (s->phase == S3_SPI_NOR_END && s->command == 0x04) {
        s->write_enable = false;
        return;
    }
    if (!s->write_enable) {
        return;
    }
    if (s->phase == S3_SPI_NOR_PROGRAM && s->program_data) {
        s->pending_base = s->page_base;
        s->pending_length = S3_SPI_NOR_PAGE_SIZE;
        s->operation = S3_SPI_NOR_PROGRAM_PENDING;
        begin_operation(s, now_ns, s->config.program_ns);
        return;
    }
    if (s->phase != S3_SPI_NOR_END) {
        return;
    }
    switch (s->command) {
    case 0x20:
        s->pending_length = 4096;
        duration = s->config.sector_erase_ns;
        break;
    case 0xd8:
        s->pending_length = 65536;
        duration = s->config.block_erase_ns;
        break;
    case 0xc7:
    case 0x60:
        s->pending_length = s->config.size_bytes;
        duration = s->config.chip_erase_ns;
        break;
    default:
        return;
    }
    s->pending_base = s->address & ~(s->pending_length - 1);
    s->operation = S3_SPI_NOR_ERASE_PENDING;
    begin_operation(s, now_ns, duration);
}

void s3_spi_nor_select(S3SPINor *s, bool active, uint64_t now_ns)
{
    s3_spi_nor_advance(s, now_ns);
    if (!s->powered || !s->memory || s->selected == active) {
        return;
    }
    if (!active) {
        finalize(s, now_ns);
    }
    reset_transaction(s);
    s->selected = active;
}

void s3_spi_nor_edge(S3SPINor *s, bool rising, bool mosi, uint64_t now_ns,
                     bool *miso, bool *driven)
{
    s3_spi_nor_advance(s, now_ns);
    if (s->powered && s->selected) {
        if (rising) {
            /* Keep the electrical line stable across the sampling edge;
             * only the following falling edge exposes the next bit. */
            if (s->response && s->driven && ++s->tx_bits == 8) {
                next_response(s);
            }
            s->rx_byte = (s->rx_byte << 1) | mosi;
            if (++s->rx_bits == 8) {
                uint8_t byte = s->rx_byte;

                s->rx_byte = 0;
                s->rx_bits = 0;
                receive_byte(s, byte);
            }
        } else {
            s->driven = s->response;
            s->miso = s->response && ((s->tx_byte >> (7 - s->tx_bits)) & 1);
        }
    }
    *miso = s->miso;
    *driven = s->driven;
}

void s3_spi_nor_abort(S3SPINor *s, uint64_t now_ns)
{
    s3_spi_nor_advance(s, now_ns);
    reset_transaction(s);
    s->selected = false;
}

void s3_spi_nor_power(S3SPINor *s, bool powered, uint64_t now_ns)
{
    s3_spi_nor_advance(s, now_ns);
    if (s->powered == powered) {
        return;
    }
    s3_spi_nor_abort(s, now_ns);
    s->write_enable = false;
    s->operation = S3_SPI_NOR_IDLE;
    s->busy_until_ns = 0;
    s->powered = powered && s->memory;
}
