/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_SPI_SERVICE_H
#define ESP32S3_SPI_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define S3_SPI_NOR_PAGE_SIZE 256u
#define S3_SPI_NOR_MAX_SIZE (16u * 1024u * 1024u)

/* This explicit 24-bit JEDEC NOR profile supports electrical SPI modes 0/3:
 * MOSI is sampled on rising SCLK and MISO changes only on falling SCLK.
 * Modes 1/2, dual/quad commands, protection, suspend and endurance are not
 * supported. A caller must not silently reinterpret those modes as mode 0.
 * Durations and all API timestamps are virtual nanoseconds. */
typedef struct S3SPINorConfig {
    uint32_t size_bytes;             /* Power of two, 64 KiB through 16 MiB. */
    uint8_t jedec_id[3];             /* Independently configurable per chip. */
    uint64_t program_ns;
    uint64_t sector_erase_ns;
    uint64_t block_erase_ns;
    uint64_t chip_erase_ns;
} S3SPINorConfig;

typedef enum S3SPINorPhase {
    S3_SPI_NOR_COMMAND,
    S3_SPI_NOR_ADDRESS,
    S3_SPI_NOR_DUMMY,
    S3_SPI_NOR_PROGRAM,
    S3_SPI_NOR_RESPONSE,
    S3_SPI_NOR_END,
    S3_SPI_NOR_INVALID,
} S3SPINorPhase;

typedef enum S3SPINorOperation {
    S3_SPI_NOR_IDLE,
    S3_SPI_NOR_PROGRAM_PENDING,
    S3_SPI_NOR_ERASE_PENDING,
} S3SPINorOperation;

/* Fixed parser/page state plus one bounded allocation at init. One instance
 * belongs to a persistent graph component, never to an SPI controller.
 * Fields are implementation state, not a substitute byte-level transport. */
typedef struct S3SPINor {
    S3SPINorConfig config;
    uint8_t *memory;
    uint8_t page[S3_SPI_NOR_PAGE_SIZE];
    S3SPINorPhase phase;
    S3SPINorOperation operation;
    uint64_t busy_until_ns;
    uint32_t address, page_base, pending_base, pending_length;
    uint16_t page_pos;
    uint8_t command, address_bytes, rx_byte, rx_bits;
    uint8_t tx_byte, tx_bits, id_pos;
    bool powered, selected, write_enable, program_data;
    bool response, miso, driven;
} S3SPINor;

/* NULL config: erased 1 MiB, EF 40 14, 0.7/45/150/2000 ms operations.
 * Invalid config/allocation failure returns false with cleanup-safe state.
 * init must not be called over a live instance without cleanup first. */
bool s3_spi_nor_init(S3SPINor *s, const S3SPINorConfig *config);
void s3_spi_nor_cleanup(S3SPINor *s);
/* Deassertion finalizes only complete, permitted commands. Duplicate CS
 * levels are not transaction boundaries. Output floats when deselected. */
void s3_spi_nor_select(S3SPINor *s, bool active, uint64_t now_ns);
/* Outputs are independent electrical MISO value and drive-enable. MOSI is
 * sampled even in response phases; it never supplies an echoed MISO value.
 * Both output pointers are required. The caller supplies actual SCLK edges. */
void s3_spi_nor_edge(S3SPINor *s, bool rising, bool mosi, uint64_t now_ns,
                     bool *miso, bool *driven);
/* Controller reset aborts an incomplete CS transaction without finalizing it;
 * committed storage, WEL and any already-started busy operation persist. */
void s3_spi_nor_abort(S3SPINor *s, uint64_t now_ns);
/* Actual component power loss aborts transactions and not-yet-complete busy
 * work, clears WEL, and preserves previously committed nonvolatile bytes.
 * Power restoration starts deselected. Duplicate levels have no effect. */
void s3_spi_nor_power(S3SPINor *s, bool powered, uint64_t now_ns);
/* Commits a pending operation iff virtual time has reached its deadline.
 * select/edge/abort/power also call this. No host time, timer or controller. */
void s3_spi_nor_advance(S3SPINor *s, uint64_t now_ns);

#endif
