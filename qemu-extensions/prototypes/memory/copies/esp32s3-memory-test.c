/* Native functional memory qualification, not cache timing or silicon proof.
 * Copyright (c) 2026 ESP32S3VM project. GPL-2.0-or-later.
 * All descriptors are internal DRAM; CPU accesses use the cache I/O alias,
 * while GDMA must reach the actual SPI1 SSI array through its own MMU alias.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define EXT 0x600c4000u
#define MMU 0x600c5000u
#define DATA 0x3c000000u
#define DMA 0x6003f000u
#define SPI 0x60002000u
#define SPI0 0x60003000u
#define OD 0x3fc88000u
#define ID 0x3fc88020u
#define SRC 0x3fc89000u
#define DST 0x3fc8a000u
#define PAGE 65536u
#define PSRAM BIT(15)
#define INVALID BIT(14)
#define INV 1u
#define WB 2u
#define CLEAN 4u

/* ESP-IDF b774170ff46c393eeb5e495ea37936038d3f4f4f:
 * soc/esp32s3/register/soc/spi_mem_reg.h and
 * hal/esp32s3/include/hal/psram_ctrlr_ll.h; phase sequence from
 * esp_psram/device/esp_psram_impl_ap_quad.c:config_psram_spi_phases().
 * Bit-length and dummy fields encode count minus one, not lane clocks.
 */
typedef enum SpiMemRegister {
    SPI_MEM_CTRL = 0x08,
    SPI_MEM_MISC = 0x34,
    SPI_MEM_CACHE_SCTRL = 0x40,
    SPI_MEM_SRAM_DRD_CMD = 0x48,
    SPI_MEM_SRAM_DWR_CMD = 0x4c,
    SPI_MEM_SRAM_CLK = 0x50,
    SPI_MEM_SMEM_DDR = 0xe4,
    SPI_MEM_CLOCK_GATE = 0xe8,
    SPI_MEM_CORE_CLK_SEL = 0xec,
} SpiMemRegister;

typedef enum SpiMemPhase {
    SPI_MEM_CS0_DIS = BIT(0),
    SPI_MEM_CS1_DIS = BIT(1),
    SPI_MEM_FCMD_QUAD = BIT(8),
    SPI_MEM_SRAM_DIO = BIT(1),
    SPI_MEM_SRAM_QIO = BIT(2),
    SPI_MEM_WR_DUMMY = BIT(3),
    SPI_MEM_RD_DUMMY = BIT(4),
    SPI_MEM_USR_RCMD = BIT(5),
    SPI_MEM_USR_WCMD = BIT(20),
    SPI_MEM_SRAM_OCT = BIT(21),
    SPI_MEM_ADDR_SHIFT = 14,
    SPI_MEM_RD_DUMMY_SHIFT = 6,
    SPI_MEM_WR_DUMMY_SHIFT = 22,
    SPI_MEM_CMD_SHIFT = 28,
    SPI_MEM_DDR_EN = BIT(0),
    SPI0_SDR_DDR = BIT(12) | BIT(13) | (1u << 5),
    SPI0_QPI_PHASES = SPI_MEM_SRAM_QIO | SPI_MEM_RD_DUMMY |
                      SPI_MEM_USR_RCMD | SPI_MEM_USR_WCMD |
                      (23u << SPI_MEM_ADDR_SHIFT) |
                      (5u << SPI_MEM_RD_DUMMY_SHIFT) |
                      (1u << SPI_MEM_WR_DUMMY_SHIFT),
} SpiMemPhase;

/* AP quad command values pinned by the same IDF device source and
 * ssi-contract.json; these exercise the SSI chip's actual mode state.
 */
typedef enum PsramCommand {
    PSRAM_READ = 0x03,
    PSRAM_WRITE = 0x02,
    PSRAM_READ_ID = 0x9f,
    PSRAM_ENTER_QPI = 0x35,
    PSRAM_EXIT_QPI = 0xf5,
    PSRAM_QPI_WRITE = 0x38,
    PSRAM_QPI_READ = 0xeb,
    PSRAM_RESET_ENABLE = 0x66,
    PSRAM_RESET = 0x99,
} PsramCommand;

static void pio(QTestState *q, unsigned cmd, bool address, uint32_t addr,
                unsigned dummy, const uint8_t *tx, uint8_t *rx, unsigned n);

static void configure_spi0(QTestState *q)
{
    qtest_writel(q, SPI0 + SPI_MEM_MISC, SPI_MEM_CS0_DIS);
    qtest_writel(q, SPI0 + SPI_MEM_CACHE_SCTRL, SPI0_QPI_PHASES);
    qtest_writel(q, SPI0 + SPI_MEM_SRAM_DRD_CMD,
                 (7u << SPI_MEM_CMD_SHIFT) | PSRAM_QPI_READ);
    qtest_writel(q, SPI0 + SPI_MEM_SRAM_DWR_CMD,
                 (7u << SPI_MEM_CMD_SHIFT) | PSRAM_QPI_WRITE);
    /* Two-way divider: N=1,H=0,L=1 per primary SRAM_CLK definitions.
     * 80MHz core selector 0 / 2 is the qualified 40MHz configuration,
     * not an electrical or cycle-accurate timing claim.
     */
    qtest_writel(q, SPI0 + SPI_MEM_SRAM_CLK, (1u << 16) | 1u);
    qtest_writel(q, SPI0 + SPI_MEM_CORE_CLK_SEL, 0);
    /* Ordinary rtc_init clears register-clock FORCE_ON; automatic gating works. */
    qtest_writel(q, SPI0 + SPI_MEM_CLOCK_GATE, 0);
    qtest_writel(q, SPI0 + SPI_MEM_SMEM_DDR, SPI0_SDR_DDR);
}

static void spi1_command_mode(QTestState *q, bool qpi)
{
    uint32_t ctrl = qtest_readl(q, SPI + SPI_MEM_CTRL);
    qtest_writel(q, SPI + SPI_MEM_CTRL,
                 qpi ? ctrl | SPI_MEM_FCMD_QUAD : ctrl & ~SPI_MEM_FCMD_QUAD);
}

static void map(QTestState *q, unsigned slot, uint32_t entry)
{
    qtest_writel(q, MMU + slot * 4, entry);
}

static void configure_cache(QTestState *q)
{
    /* Enable, 32-byte lines, 32KiB; way bit is deliberately left clear. */
    qtest_writel(q, EXT, 1 | 8);
    qtest_writel(q, EXT + 4, 0); /* Open both CPU data-cache buses. */
    map(q, 0, PSRAM);
}

/* Reconfiguration is also used after warm reset: external QPI persists.
 * Sending another 35 here would be an invalid command in that state.
 */
static void configure(QTestState *q)
{
    configure_spi0(q);
    configure_cache(q);
}

static QTestState *start(void)
{
    QTestState *q = qtest_init("-M esp32s3 -m 8M -S");
    pio(q, PSRAM_ENTER_QPI, false, 0, 0, NULL, NULL, 0);
    spi1_command_mode(q, true);
    configure(q);
    return q;
}

static void pattern(uint8_t *p, unsigned n, unsigned seed)
{
    for (unsigned i = 0; i < n; i++) {
        p[i] = (i * 73 + (i >> 2) * 19 + seed) ^ (i >> 1);
    }
}

static void equal(QTestState *q, uint32_t addr, const uint8_t *p, unsigned n)
{
    uint8_t actual[256];
    g_assert_cmpuint(n, <=, sizeof(actual));
    qtest_memread(q, addr, actual, n);
    g_assert_cmpmem(actual, n, p, n);
}

static void desc(QTestState *q, uint32_t at, unsigned size, unsigned len,
                 bool eof, uint32_t buffer)
{
    /* Explicit word writes avoid host endianness in descriptor encoding. */
    qtest_writel(q, at, size | (len << 12) | BIT(31) | (eof ? BIT(30) : 0));
    qtest_writel(q, at + 4, buffer);
    qtest_writel(q, at + 8, 0);
}

static uint32_t transfer(QTestState *q, uint32_t src, uint32_t dst,
                         unsigned len, unsigned size, int block)
{
    /* Reset each channel direction and retire all old status. */
    qtest_writel(q, DMA, 1);
    qtest_writel(q, DMA + 0x60, 1);
    qtest_writel(q, DMA, BIT(4) | (block >= 0 ? BIT(2) : 0));
    qtest_writel(q, DMA + 0x60, BIT(2));
    qtest_writel(q, DMA + 4, BIT(12) | (block >= 0 ? block << 13 : 0));
    qtest_writel(q, DMA + 0x64, BIT(12));
    qtest_writel(q, DMA + 0x14, UINT32_MAX);
    qtest_writel(q, DMA + 0x74, UINT32_MAX);
    desc(q, OD, len, len, true, src);
    desc(q, ID, size, 0, false, dst);
    qtest_writel(q, DMA + 0x20, (ID & 0xfffff) | BIT(22));
    qtest_writel(q, DMA + 0x80, (OD & 0xfffff) | BIT(21));
    return (qtest_readl(q, DMA + 8) & BIT(3)) |
           (qtest_readl(q, DMA + 0x68) & BIT(2));
}

static void move(QTestState *q, uint32_t src, uint32_t dst, unsigned n)
{
    g_assert_cmphex(transfer(q, src, dst, n, n, -1), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 8), ==, 3);
    g_assert_cmphex(qtest_readl(q, ID) & BIT(31), ==, 0);
    g_assert_cmpuint((qtest_readl(q, ID) >> 12) & 0xfff, ==, n);
}

static void cache_sync(QTestState *q, uint32_t addr, unsigned blocks, unsigned op)
{
    qtest_writel(q, EXT + 0x2c, addr);
    qtest_writel(q, EXT + 0x30, blocks);
    qtest_writel(q, EXT + 0x28, op);
    g_assert_cmphex(qtest_readl(q, EXT + 0x28), ==, BIT(3));
}

static void test_visibility(void)
{
    QTestState *q = start();
    uint8_t a[64], b[64], c[64];
    pattern(a, sizeof(a), 17);
    pattern(b, sizeof(b), 91);
    pattern(c, sizeof(c), 203);
    qtest_memwrite(q, SRC, a, sizeof(a));
    move(q, SRC, DATA, sizeof(a));
    equal(q, DATA, a, sizeof(a));
    qtest_memwrite(q, DATA, b, sizeof(b));
    equal(q, DATA, b, sizeof(b));
    move(q, DATA, DST, sizeof(a));
    equal(q, DST, a, sizeof(a)); /* CPU dirty data is not committed SSI. */
    cache_sync(q, DATA, 2, WB);
    move(q, DATA, DST, sizeof(b));
    equal(q, DST, b, sizeof(b));
    qtest_memwrite(q, SRC, c, sizeof(c));
    move(q, SRC, DATA, sizeof(c));
    equal(q, DATA, b, sizeof(b)); /* DMA cannot silently refresh CPU lines. */
    cache_sync(q, DATA, 2, INV);
    equal(q, DATA, c, sizeof(c));
    qtest_memwrite(q, DATA, a, sizeof(a));
    cache_sync(q, DATA, 2, INV); /* Invalidate alone discards dirty data. */
    equal(q, DATA, c, sizeof(c));
    qtest_memwrite(q, DATA, b, sizeof(b));
    cache_sync(q, DATA, 2, CLEAN);
    move(q, DATA, DST, sizeof(b));
    equal(q, DST, c, sizeof(c)); /* Clean clears dirty, not writeback. */
    equal(q, DATA, b, sizeof(b));
    cache_sync(q, DATA, 2, WB);
    move(q, DATA, DST, sizeof(c));
    equal(q, DST, c, sizeof(c)); /* Cleaned bytes must not later commit. */
    qtest_memwrite(q, SRC, a, sizeof(a));
    move(q, SRC, DATA, sizeof(a));
    cache_sync(q, DATA, 2, INV);
    equal(q, DATA, a, sizeof(a)); /* Clean did not leave dirty writeback. */
    qtest_memwrite(q, DATA, c, sizeof(c));
    cache_sync(q, 0, 0, WB | INV); /* ROM whole-cache convention, not a no-op. */
    move(q, DATA, DST, sizeof(c));
    equal(q, DST, c, sizeof(c));
    equal(q, DATA, c, sizeof(c));
    qtest_quit(q);
}

static void test_alias_remap(void)
{
    QTestState *q = start();
    uint8_t a[32], b[32];
    pattern(a, sizeof(a), 37);
    pattern(b, sizeof(b), 149);
    map(q, 1, PSRAM);
    qtest_memwrite(q, DATA, a, sizeof(a));
    equal(q, DATA + PAGE, a, sizeof(a));
    cache_sync(q, DATA + PAGE, 1, WB);
    move(q, DATA + PAGE, DST, sizeof(a));
    equal(q, DST, a, sizeof(a));
    map(q, 2, PSRAM | 1);
    qtest_memwrite(q, SRC, b, sizeof(b));
    move(q, SRC, DATA + 2 * PAGE, sizeof(b));
    map(q, 0, PSRAM | 1);
    equal(q, DATA, b, sizeof(b));
    map(q, 0, PSRAM);
    equal(q, DATA, a, sizeof(a));
    qtest_quit(q);
}

static void test_boundaries(void)
{
    QTestState *q = start();
    uint8_t p[64];
    pattern(p, sizeof(p), 71);
    map(q, 1, PSRAM | 1);
    qtest_memwrite(q, SRC, p, sizeof(p));
    move(q, SRC, DATA + PAGE - 32, sizeof(p));
    equal(q, DATA + PAGE - 32, p, sizeof(p));
    /* An eight-byte OUT beat straddles two non-contiguous physical pages. */
    map(q, 1, PSRAM | 5);
    qtest_memwrite(q, DATA + PAGE - 4, p, sizeof(p));
    cache_sync(q, DATA + PAGE - 32, 3, WB);
    move(q, DATA + PAGE - 4, DST, sizeof(p));
    equal(q, DST, p, sizeof(p));
    map(q, 2, PSRAM | 127);
    move(q, SRC, DATA + 3 * PAGE - 64, sizeof(p));
    equal(q, DATA + 3 * PAGE - 64, p, sizeof(p));
    map(q, 3, PSRAM | 128); /* Beyond real 8MiB, never modulo-wrap MMU. */
    g_assert_cmphex(transfer(q, DATA + 3 * PAGE, DST, 32, 32, -1), !=, 0);
    g_assert_cmphex(transfer(q, SRC, DATA + 3 * PAGE, 32, 32, -1), !=, 0);
    map(q, 3, INVALID);
    g_assert_cmphex(transfer(q, DATA + 3 * PAGE, DST, 32, 32, -1), !=, 0);
    map(q, 3, 0); /* Flash type is not PSRAM even with physical page zero. */
    g_assert_cmphex(transfer(q, SRC, DATA + 3 * PAGE, 32, 32, -1), !=, 0);
    /* Upper SRAM2 is occupied by the enabled 32KiB DCache, not guest RAM. */
    g_assert_cmphex(transfer(q, 0x3fcf8000u, DST, 32, 32, -1), !=, 0);
    g_assert_cmphex(transfer(q, SRC, 0x3fcf8000u, 32, 32, -1), !=, 0);
    qtest_quit(q);
}

static void test_absent(void)
{
    QTestState *q = qtest_init("-M esp32s3 -m 0 -S");
    pio(q, PSRAM_ENTER_QPI, false, 0, 0, NULL, NULL, 0);
    configure(q);
    g_assert_cmphex(transfer(q, DATA, DST, 32, 32, -1), !=, 0);
    g_assert_cmphex(transfer(q, SRC, DATA, 32, 32, -1), !=, 0);
    qtest_quit(q);
}

static void test_alignment(void)
{
    QTestState *q = start();
    uint8_t p[64];
    pattern(p, sizeof(p), 55);
    qtest_memwrite(q, SRC, p, sizeof(p));
    for (int bk = 0; bk < 3; bk++) {
        unsigned size = 16u << bk;
        g_assert_cmphex(transfer(q, SRC, DATA + 1, 7, size, bk), !=, 0);
        g_assert_cmphex(qtest_readl(q, DMA + 8) & BIT(3), !=, 0);
        g_assert_cmphex(transfer(q, SRC, DATA, 7, size - 1, bk), !=, 0);
        g_assert_cmphex(qtest_readl(q, DMA + 8) & BIT(3), !=, 0);
        /* Length is deliberately not block-aligned: only IN size/address. */
        g_assert_cmphex(transfer(q, SRC + 1, DATA, 7, size, bk), ==, 0);
        cache_sync(q, DATA, 2, INV);
        equal(q, DATA, p + 1, 7);
    }
    qtest_quit(q);
}

static void test_reset(void)
{
    QTestState *q = start();
    uint8_t a[32], b[32];
    pattern(a, sizeof(a), 109);
    pattern(b, sizeof(b), 211);
    qtest_memwrite(q, SRC, a, sizeof(a));
    move(q, SRC, DATA, sizeof(a));
    qtest_memwrite(q, DATA, b, sizeof(b));
    qtest_qmp_assert_success(q, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(q, "RESET");
    for (unsigned i = 0; i < 512; i++) {
        g_assert_cmphex(qtest_readl(q, MMU + i * 4), ==, INVALID);
    }
    g_assert_cmphex(transfer(q, DATA, DST, 32, 32, -1), !=, 0);
    /* Restore MMU/cache only first: valid translation cannot bypass the
     * reset SPI0 phase registers, although the external chip remains QPI.
     */
    configure_cache(q);
    g_assert_cmphex(transfer(q, DATA, DST, 32, 32, -1), ==, BIT(2));
    g_assert_cmphex(transfer(q, SRC, DATA, 32, 32, -1), ==, BIT(3));
    configure_spi0(q);
    equal(q, DATA, a, sizeof(a));
    move(q, DATA, DST, sizeof(a));
    equal(q, DST, a, sizeof(a));
    qtest_quit(q);
}

/* Ordinary SPI1 USR transactions select CS1 only, never flash/XTS. */
static void pio(QTestState *q, unsigned cmd, bool address, uint32_t addr,
                unsigned dummy, const uint8_t *tx, uint8_t *rx, unsigned n)
{
    uint32_t user = BIT(31) | (address ? BIT(30) : 0) |
                    (dummy ? BIT(29) : 0) | (tx ? BIT(27) : 0) |
                    (rx ? BIT(28) : 0);
    g_assert_cmpuint(n, <=, 64);
    qtest_writel(q, SPI + SPI_MEM_MISC, SPI_MEM_CS0_DIS);
    for (unsigned i = 0; i < 16; i++) {
        uint32_t word = 0;
        for (unsigned j = 0; tx && j < 4 && i * 4 + j < n; j++) {
            word |= (uint32_t)tx[i * 4 + j] << (j * 8);
        }
        qtest_writel(q, SPI + 0x58 + 4 * i, word);
    }
    /* SPI_MEM_ADDR.USR_ADDR_VALUE is the full 32-bit field (bitpos[31:0],
     * shift 0 in the pinned register definition); the controller sends its top
     * USR_ADDR_BITLEN+1 bits MSB first. */
    qtest_writel(q, SPI + 4, addr);
    qtest_writel(q, SPI + 0x18, user);
    qtest_writel(q, SPI + 0x1c, (23u << 26) | (dummy ? dummy - 1 : 0));
    qtest_writel(q, SPI + 0x20, (7u << 28) | cmd);
    qtest_writel(q, SPI + 0x24, n ? n * 8 - 1 : 0);
    qtest_writel(q, SPI + 0x28, n ? n * 8 - 1 : 0);
    qtest_writel(q, SPI, BIT(18));
    for (unsigned i = 0; rx && i < n; i++) {
        rx[i] = qtest_readl(q, SPI + 0x58 + (i / 4) * 4) >> ((i % 4) * 8);
    }
}

static void duplex_read(QTestState *q, uint32_t addr, unsigned tx_len,
                        uint8_t *rx, unsigned rx_len)
{
    g_assert_cmpuint(tx_len, >, 0);
    g_assert_cmpuint(tx_len, <=, 64);
    g_assert_cmpuint(rx_len, >, 0);
    g_assert_cmpuint(rx_len, <=, 64);
    qtest_writel(q, SPI + SPI_MEM_MISC, SPI_MEM_CS0_DIS);
    /* Every stale FIFO byte exceeds either logical length. Old byte-value
     * bounds skip RX, retaining these clock bytes rather than SSI data. */
    for (unsigned i = 0; i < 16; i++) {
        qtest_writel(q, SPI + 0x58 + i * 4, 0xe3c7a589u);
    }
    qtest_writel(q, SPI + 4, addr);
    qtest_writel(q, SPI + 0x18, BIT(31) | BIT(30) | BIT(28) | BIT(27));
    qtest_writel(q, SPI + 0x1c, 23u << 26);
    qtest_writel(q, SPI + 0x20, (7u << 28) | PSRAM_READ);
    qtest_writel(q, SPI + 0x24, tx_len * 8 - 1);
    qtest_writel(q, SPI + 0x28, rx_len * 8 - 1);
    qtest_writel(q, SPI, BIT(18));
    for (unsigned i = 0; i < rx_len; i++) {
        rx[i] = qtest_readl(q, SPI + 0x58 + (i / 4) * 4) >> ((i % 4) * 8);
    }
}

static void test_spi1_transport(unsigned tx_len)
{
    QTestState *q = qtest_init("-M esp32s3 -m 8M -S");
    uint8_t p[32], got[32];
    pattern(p, sizeof(p), 197);
    pio(q, PSRAM_WRITE, true, 0x200, 0, p, NULL, sizeof(p));
    duplex_read(q, 0x200, tx_len, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), p, sizeof(p));
    qtest_quit(q);
}

static void test_spi1_high_duplex(void)
{
    test_spi1_transport(32);
}

static void test_spi1_asymmetric(void)
{
    test_spi1_transport(2);
}


static void test_spi1_ssi(void)
{
    QTestState *q = qtest_init("-M esp32s3 -m 8M -S");
    const uint8_t id[8] = { 0x0d, 0x5d, 0x42, 0, 0, 0, 0, 0 };
    uint8_t got[32], p[32];
    pio(q, PSRAM_READ_ID, true, 0, 0, NULL, got, 8);
    g_assert_cmpmem(got, 8, id, 8);
    /* Eight FIFO transactions cover all source bytes. Read-only transfers
     * also exercise unequal RX/TX lengths (RX=32, TX=0). */
    for (unsigned chunk = 0; chunk < 8; chunk++) {
        for (unsigned i = 0; i < sizeof(p); i++) {
            p[i] = chunk * sizeof(p) + i;
        }
        pio(q, PSRAM_WRITE, true, 0x200 + chunk * sizeof(p), 0,
            p, NULL, sizeof(p));
        pio(q, PSRAM_READ, true, 0x200 + chunk * sizeof(p), 0,
            NULL, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), p, sizeof(p));
    }
    pattern(p, sizeof(p), 197);
    pio(q, PSRAM_ENTER_QPI, false, 0, 0, NULL, NULL, 0);
    spi1_command_mode(q, true);
    pio(q, PSRAM_QPI_WRITE, true, 0x100, 0, p, NULL, sizeof(p));
    pio(q, PSRAM_QPI_READ, true, 0x100, 6, NULL, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), p, sizeof(p));
    configure(q); /* Chip is already QPI: program SPI0 without another 35. */
    move(q, DATA + 0x100, DST, sizeof(p));
    equal(q, DST, p, sizeof(p)); /* SSI protocol and GDMA share backing. */
    equal(q, DATA + 0x100, p, sizeof(p));
    /* Warm reset does not power-cycle external PSRAM or exit QPI. */
    qtest_qmp_assert_success(q, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(q, "RESET");
    spi1_command_mode(q, true); /* SPI1 resets, but external QPI does not. */
    configure(q);
    pio(q, PSRAM_QPI_READ, true, 0x100, 6, NULL, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), p, sizeof(p));
    move(q, DATA + 0x100, DST, sizeof(p));
    equal(q, DST, p, sizeof(p));
    /* F5 is a genuine QPI->SPI transition, not a mode-flag readback. */
    pio(q, PSRAM_EXIT_QPI, false, 0, 0, NULL, NULL, 0);
    spi1_command_mode(q, false);
    pio(q, PSRAM_READ_ID, true, 0, 0, NULL, got, 8);
    g_assert_cmpmem(got, 8, id, 8);
    pio(q, PSRAM_READ, true, 0x100, 0, NULL, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), p, sizeof(p));
    g_assert_cmphex(transfer(q, DATA + 0x100, DST, 32, 32, -1), ==, BIT(2));
    g_assert_cmphex(transfer(q, SRC, DATA + 0x100, 32, 32, -1), ==, BIT(3));
    pio(q, PSRAM_ENTER_QPI, false, 0, 0, NULL, NULL, 0);
    spi1_command_mode(q, true);
    move(q, DATA + 0x100, DST, sizeof(p));
    equal(q, DST, p, sizeof(p));
    /* 99 without a preceding 66 must not exit QPI or erase stored bytes. */
    pio(q, PSRAM_RESET, false, 0, 0, NULL, NULL, 0);
    pio(q, PSRAM_QPI_READ, true, 0x100, 6, NULL, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), p, sizeof(p));
    move(q, DATA + 0x100, DST, sizeof(p));
    equal(q, DST, p, sizeof(p));
    pio(q, PSRAM_RESET_ENABLE, false, 0, 0, NULL, NULL, 0);
    pio(q, PSRAM_RESET, false, 0, 0, NULL, NULL, 0);
    spi1_command_mode(q, false);
    pio(q, PSRAM_READ_ID, true, 0, 0, NULL, got, 8);
    g_assert_cmpmem(got, 8, id, 8);
    pio(q, PSRAM_READ, true, 0x100, 0, NULL, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), p, sizeof(p));
    g_assert_cmphex(transfer(q, DATA + 0x100, DST, 32, 32, -1), ==, BIT(2));
    g_assert_cmphex(transfer(q, SRC, DATA + 0x100, 32, 32, -1), ==, BIT(3));
    pio(q, PSRAM_ENTER_QPI, false, 0, 0, NULL, NULL, 0);
    spi1_command_mode(q, true);
    move(q, DATA + 0x100, DST, sizeof(p));
    equal(q, DST, p, sizeof(p));
    qtest_quit(q);
}

static void test_spi0_phases(void)
{
    /* Same IDF pin: soc/esp32s3/register/soc/extmem_reg.h.
     * SYNC_DONE reports command completion, not successful physical WB.
     */
    enum {
        CACHE_ILG_INT_CLR = 0xe0,
        CACHE_ILG_INT_ST = 0xe4,
        DCACHE_SYNC_OP_FAULT = BIT(2),
    };
    static const struct {
        const char *name;
        SpiMemRegister reg;
        uint32_t value;
        bool read_bad;
        bool write_bad;
    } invalid[] = {
        { "CS1-disabled", SPI_MEM_MISC,
          SPI_MEM_CS0_DIS | SPI_MEM_CS1_DIS, true, true },
        { "CS0-instead-of-CS1", SPI_MEM_MISC, SPI_MEM_CS1_DIS, true, true },
        { "serial-read-opcode", SPI_MEM_SRAM_DRD_CMD,
          (7u << SPI_MEM_CMD_SHIFT) | PSRAM_READ, true, false },
        { "serial-write-opcode", SPI_MEM_SRAM_DWR_CMD,
          (7u << SPI_MEM_CMD_SHIFT) | PSRAM_WRITE, false, true },
        { "read-command-seven-bits", SPI_MEM_SRAM_DRD_CMD,
          (6u << SPI_MEM_CMD_SHIFT) | PSRAM_QPI_READ, true, false },
        { "write-command-seven-bits", SPI_MEM_SRAM_DWR_CMD,
          (6u << SPI_MEM_CMD_SHIFT) | PSRAM_QPI_WRITE, false, true },
        { "address-twenty-three-bits", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES ^ (1u << SPI_MEM_ADDR_SHIFT), true, true },
        { "read-dummy-disabled", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES & ~SPI_MEM_RD_DUMMY, true, false },
        { "read-dummy-five-clocks", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES ^ (1u << SPI_MEM_RD_DUMMY_SHIFT), true, false },
        { "write-dummy-enabled", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES | SPI_MEM_WR_DUMMY, false, true },
        { "user-read-command-disabled", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES & ~SPI_MEM_USR_RCMD, true, false },
        { "user-write-command-disabled", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES & ~SPI_MEM_USR_WCMD, false, true },
        { "QPI-flag-cleared", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES & ~SPI_MEM_SRAM_QIO, true, true },
        { "dual-mode-selected", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES | SPI_MEM_SRAM_DIO, true, true },
        { "octal-mode-selected", SPI_MEM_CACHE_SCTRL,
          SPI0_QPI_PHASES | SPI_MEM_SRAM_OCT, true, true },
        { "DDR-enabled", SPI_MEM_SMEM_DDR,
          SPI0_SDR_DDR | SPI_MEM_DDR_EN, true, true },
        { "wrong-clock-divider", SPI_MEM_SRAM_CLK,
          (3u << 16) | (1u << 8) | 3u, true, true },
        { "wrong-core-clock-selector", SPI_MEM_CORE_CLK_SEL, 1, true, true },
    };
    QTestState *q = start();
    uint8_t committed[32], replacement[32], guard[32];
    pattern(committed, sizeof(committed), 47);
    pattern(replacement, sizeof(replacement), 131);
    pattern(guard, sizeof(guard), 229);
    /* CLK_EN is register-clock FORCE_ON, not the functional SPI01 gate.
     * Both override settings must transport real committed bytes. */
    for (unsigned force = 0; force <= 1; force++) {
        uint8_t actual[32];
        configure_spi0(q);
        qtest_writel(q, SPI0 + SPI_MEM_CLOCK_GATE, force);
        pio(q, PSRAM_QPI_WRITE, true, 0x600, 0,
            committed, NULL, sizeof(committed));
        move(q, DATA + 0x600, DST, sizeof(committed));
        equal(q, DST, committed, sizeof(committed));
        qtest_memwrite(q, SRC, replacement, sizeof(replacement));
        move(q, SRC, DATA + 0x600, sizeof(replacement));
        pio(q, PSRAM_QPI_READ, true, 0x600, 6,
            NULL, actual, sizeof(actual));
        g_assert_cmpmem(actual, sizeof(actual), replacement, sizeof(replacement));
    }

    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_test_message("SPI0 phase consumer: %s", invalid[i].name);
        /* Known bytes arrive through the sole SSI chip, never a RAM alias.
         * MMU/type/capacity and the external QPI state remain valid.
         */
        configure_spi0(q);
        pio(q, PSRAM_QPI_WRITE, true, 0x600, 0,
            committed, NULL, sizeof(committed));
        qtest_memwrite(q, SRC, replacement, sizeof(replacement));
        qtest_memwrite(q, DST, guard, sizeof(guard));
        qtest_writel(q, SPI0 + invalid[i].reg, invalid[i].value);

        if (invalid[i].read_bad) {
            g_assert_cmphex(transfer(q, DATA + 0x600, DST, 32, 32, -1),
                            ==, BIT(2)); /* Native OUT_DSCR_ERR. */
            equal(q, DST, guard, sizeof(guard));
        } else {
            move(q, DATA + 0x600, DST, sizeof(committed));
            equal(q, DST, committed, sizeof(committed));
        }
        if (invalid[i].write_bad) {
            g_assert_cmphex(transfer(q, SRC, DATA + 0x600, 32, 32, -1),
                            ==, BIT(3)); /* Native IN_DSCR_ERR. */
        } else {
            move(q, SRC, DATA + 0x600, sizeof(replacement));
        }
        /* Restoring the real phase registers resumes byte transport.
         * Failed stores must not secretly change SSI storage; a bad read
         * phase must not suppress an otherwise valid write phase.
         */
        configure_spi0(q);
        move(q, DATA + 0x600, DST, sizeof(committed));
        equal(q, DST, invalid[i].write_bad ? committed : replacement,
              sizeof(committed));
        cache_sync(q, DATA + 0x600, 1, INV);
        equal(q, DATA + 0x600,
              invalid[i].write_bad ? committed : replacement,
              sizeof(committed));
        if (invalid[i].write_bad) {
            /* A failed physical WB must retain dirty CPU bytes for retry,
             * rather than claiming success or silently dropping the line.
             */
            qtest_memwrite(q, DATA + 0x600, replacement, sizeof(replacement));
            qtest_writel(q, EXT + CACHE_ILG_INT_CLR, DCACHE_SYNC_OP_FAULT);
            g_assert_cmphex(qtest_readl(q, EXT + CACHE_ILG_INT_ST) &
                            DCACHE_SYNC_OP_FAULT, ==, 0);
            qtest_writel(q, SPI0 + invalid[i].reg, invalid[i].value);
            /* Cached hits need no external transaction, even with a bad
             * physical phase. Their dirty bytes must survive failed WB.
             */
            equal(q, DATA + 0x600, replacement, sizeof(replacement));
            cache_sync(q, DATA + 0x600, 1, WB);
            g_assert_cmphex(qtest_readl(q, EXT + CACHE_ILG_INT_ST) &
                            DCACHE_SYNC_OP_FAULT, ==, DCACHE_SYNC_OP_FAULT);
            equal(q, DATA + 0x600, replacement, sizeof(replacement));
            qtest_writel(q, EXT + CACHE_ILG_INT_CLR, DCACHE_SYNC_OP_FAULT);
            g_assert_cmphex(qtest_readl(q, EXT + CACHE_ILG_INT_ST) &
                            DCACHE_SYNC_OP_FAULT, ==, 0);
            configure_spi0(q);
            move(q, DATA + 0x600, DST, sizeof(committed));
            equal(q, DST, committed, sizeof(committed));
            cache_sync(q, DATA + 0x600, 1, WB);
            g_assert_cmphex(qtest_readl(q, EXT + CACHE_ILG_INT_ST) &
                            DCACHE_SYNC_OP_FAULT, ==, 0);
            move(q, DATA + 0x600, DST, sizeof(replacement));
            equal(q, DST, replacement, sizeof(replacement));
        }
    }
    qtest_quit(q);
}

static void test_spi0_clock_reset(void)
{
    /* ESP-IDF b774170ff46c393eeb5e495ea37936038d3f4f4f:
     * soc/esp32s3/register/soc/reg_base.h and system_reg.h.
     * SPI01 has one shared SYSTEM gate/reset bit, not guessed SPI0 ports.
     */
    enum {
        SYSTEM_BASE = 0x600c0000,
        SYSTEM_PERIP_CLK_EN0 = 0x18,
        SYSTEM_PERIP_RST_EN0 = 0x20,
        SYSTEM_SPI01 = BIT(1),
    };
    QTestState *q = start();
    uint8_t committed[32], dirty[32], guard[32], got[32];
    uint32_t clocks = qtest_readl(q, SYSTEM_BASE + SYSTEM_PERIP_CLK_EN0);
    uint32_t resets = qtest_readl(q, SYSTEM_BASE + SYSTEM_PERIP_RST_EN0);

    pattern(committed, sizeof(committed), 61);
    pattern(dirty, sizeof(dirty), 173);
    pattern(guard, sizeof(guard), 241);
    qtest_memwrite(q, SRC, committed, sizeof(committed));
    move(q, SRC, DATA, sizeof(committed));
    equal(q, DATA, committed, sizeof(committed));
    qtest_memwrite(q, DATA, dirty, sizeof(dirty));
    qtest_memwrite(q, DST, guard, sizeof(guard));

    qtest_writel(q, SYSTEM_BASE + SYSTEM_PERIP_CLK_EN0,
                 clocks & ~SYSTEM_SPI01);
    g_assert_cmphex(transfer(q, DATA, DST, 32, 32, -1), ==, BIT(2));
    equal(q, DST, guard, sizeof(guard));
    g_assert_cmphex(transfer(q, SRC, DATA, 32, 32, -1), ==, BIT(3));
    equal(q, DATA, dirty, sizeof(dirty)); /* Cache hits need no SPI clock. */
    qtest_writel(q, SYSTEM_BASE + SYSTEM_PERIP_CLK_EN0,
                 clocks | SYSTEM_SPI01);
    /* Gate cycling loses neither actual phase configuration nor storage. */
    move(q, DATA, DST, sizeof(committed));
    equal(q, DST, committed, sizeof(committed));
    equal(q, DATA, dirty, sizeof(dirty));

    qtest_writel(q, SYSTEM_BASE + SYSTEM_PERIP_RST_EN0,
                 resets | SYSTEM_SPI01);
    g_assert_cmphex(transfer(q, DATA, DST, 32, 32, -1), ==, BIT(2));
    g_assert_cmphex(transfer(q, SRC, DATA, 32, 32, -1), ==, BIT(3));
    equal(q, DATA, dirty, sizeof(dirty));
    /* Actual SPI0 writes while reset is held must not arm cold phases.
     * Prove this through consumers after release, not mode readback.
     */
    configure_spi0(q);
    qtest_writel(q, SYSTEM_BASE + SYSTEM_PERIP_RST_EN0,
                 resets & ~SYSTEM_SPI01);
    g_assert_cmphex(transfer(q, DATA, DST, 32, 32, -1), ==, BIT(2));
    g_assert_cmphex(transfer(q, SRC, DATA, 32, 32, -1), ==, BIT(3));
    equal(q, DATA, dirty, sizeof(dirty));

    configure_spi0(q);
    spi1_command_mode(q, true); /* SPI1 reset did not power-cycle the chip. */
    pio(q, PSRAM_QPI_READ, true, 0, 6, NULL, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), committed, sizeof(committed));
    move(q, DATA, DST, sizeof(committed));
    equal(q, DST, committed, sizeof(committed));
    equal(q, DATA, dirty, sizeof(dirty));
    /* Controller reset retains cache dirty state and translations, unlike
     * global peripheral/cache reset. Restored phases can really commit it.
     */
    cache_sync(q, DATA, 1, WB);
    move(q, DATA, DST, sizeof(dirty));
    equal(q, DST, dirty, sizeof(dirty));
    qtest_quit(q);
}

static void test_cache_geometry(void)
{
    static const struct {
        uint32_t control;
        unsigned line_bytes;
    } modes[] = {
        { 1, 16 }, { 1 | 8, 32 }, { 1 | 16, 64 },
        { 1 | 4 | 8, 32 }, { 1 | 4 | 16, 64 },
    };
    QTestState *q = start();
    uint8_t old[64], fresh[64];

    for (unsigned mode = 0; mode < G_N_ELEMENTS(modes); mode++) {
        g_test_message("DCache control=0x%x, line=%u",
                       modes[mode].control, modes[mode].line_bytes);
        cache_sync(q, 0, 0, INV);
        qtest_writel(q, EXT, modes[mode].control);
        pattern(old, sizeof(old), 31 + mode);
        pattern(fresh, sizeof(fresh), 137 + mode);
        pio(q, PSRAM_QPI_WRITE, true, 0x100, 0, old, NULL, sizeof(old));
        qtest_memwrite(q, DATA + 0x100, fresh, sizeof(fresh));
        move(q, DATA + 0x100, DST, sizeof(old));
        equal(q, DST, old, sizeof(old));
        equal(q, DATA + 0x100, fresh, sizeof(fresh));
    cache_sync(q, DATA + 0x100, sizeof(fresh) / modes[mode].line_bytes, WB);
        move(q, DATA + 0x100, DST, sizeof(fresh));
        equal(q, DST, fresh, sizeof(fresh));
        qtest_memwrite(q, SRC, old, sizeof(old));
        move(q, SRC, DATA + 0x100, sizeof(old));
        equal(q, DATA + 0x100, fresh, sizeof(fresh));
    cache_sync(q, DATA + 0x100, sizeof(old) / modes[mode].line_bytes, INV);
        equal(q, DATA + 0x100, old, sizeof(old));
        if (modes[mode].control & BIT(2)) {
            g_assert_cmphex(transfer(q, SRC, 0x3fcf0000u, 32, 32, -1), !=, 0);
            g_assert_cmphex(transfer(q, 0x3fcf0000u, DST, 32, 32, -1), !=, 0);
        } else {
            move(q, SRC, 0x3fcf0000u, 32);
            equal(q, 0x3fcf0000u, old, 32);
        }
        g_assert_cmphex(transfer(q, SRC, 0x3fcf8000u, 32, 32, -1), !=, 0);
    }
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3-memory/visibility", test_visibility);
    qtest_add_func("/esp32s3-memory/alias-remap", test_alias_remap);
    qtest_add_func("/esp32s3-memory/boundaries", test_boundaries);
    qtest_add_func("/esp32s3-memory/absent", test_absent);
    qtest_add_func("/esp32s3-memory/alignment", test_alignment);
    qtest_add_func("/esp32s3-memory/reset", test_reset);
    qtest_add_func("/esp32s3-memory/spi1-ssi", test_spi1_ssi);
    qtest_add_func("/esp32s3-memory/spi1-high-duplex", test_spi1_high_duplex);
    qtest_add_func("/esp32s3-memory/spi1-asymmetric", test_spi1_asymmetric);
    qtest_add_func("/esp32s3-memory/spi0-phases", test_spi0_phases);
    qtest_add_func("/esp32s3-memory/spi0-clock-reset", test_spi0_clock_reset);
    qtest_add_func("/esp32s3-memory/cache-geometry", test_cache_geometry);
    return g_test_run();
}
