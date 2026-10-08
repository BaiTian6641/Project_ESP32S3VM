/*
 * ESP32-S3 GPIO / IO_MUX / RTC_IO register-level qtest
 *
 * Exercises the H-NET-02 models against the TRM chapter 6 semantics:
 * W1TS/W1TC registers, IN/OUT/ENABLE banks, simple GPIO loopback through
 * the resolved pad nets (open-drain + pulls + floating policy), GPIO
 * matrix output selectors, GPIO_PINn interrupt types feeding the masked
 * status registers, IO_MUX function select disconnecting the matrix, and
 * the RTC_IO [31:10] register file.  IRQ end-to-end delivery (GPIO ->
 * interrupt matrix -> CPU) is covered by the gpio_net firmware fixture.
 *
 * Copyright (c) 2026 ESP32S3VM project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "hw/gpio/esp32s3_gpio.h"
#if defined(__has_include)
#if __has_include("hw/xtensa/esp32s3_reset_domain.h")
/* Present only on the native-foundations prefix, whose intmatrix model
 * exposes the INTR_STATUS + cpu-int-out observability used below. */
#include "hw/xtensa/esp32s3_reset_domain.h"
#define GPIO_TEST_HAS_MATRIX_DELIVERY 1
#endif
#endif
#include "hw/gpio/esp32s3_iomux.h"
#include "hw/misc/esp32s3_rtc_io.h"

#define GPIO_BASE   0x60004000ULL
#define IOMUX_BASE  0x60009000ULL
#define RTCIO_BASE  0x60008400ULL

#define GPIO(x)  (GPIO_BASE + (x))
#define IOMUX(x) (IOMUX_BASE + (x))
#define RTCIO(x) (RTCIO_BASE + (x))

#define PIN_REG(n)     GPIO(GPIO_PINn_REG_OFFSET(n))
#define FUNC_IN(n)     GPIO(GPIO_FUNC_IN_SEL_CFG_OFFSET(n))
#define FUNC_OUT(n)    GPIO(GPIO_FUNC_OUT_SEL_CFG_OFFSET(n))
#define IO_MUX_PAD(n)  IOMUX(IO_MUX_GPIOn_REG_OFFSET(n))

#define RTC_PAD_CFG(n) RTCIO(RTC_IO_PAD_OFF(n))

static QTestState *s;

/* PIN_FUNC_GPIO (1 on the S3) + FUN_IE (bit 9): pad on the GPIO matrix,
 * input enabled */
#define IOMUX_GPIO_IN_EN  (R_IO_MUX_GPIOn_FUN_IE_MASK | \
                           (ESP32S3_IOMUX_MCU_SEL_GPIO << R_IO_MUX_GPIOn_MCU_SEL_SHIFT))

static void drive_input(unsigned pad, int level)
{
    qtest_set_irq_in(s, "/machine/soc/gpio", ESP32S3_GPIO_INPUT_LINES,
                     (int)pad, level);
}

static void drive_rtc_input(unsigned pad, int level)
{
    qtest_set_irq_in(s, "/machine/soc/rtc-io", ESP32S3_RTC_IO_INPUT_LINES,
                     (int)pad, level);
}

static void test_reset_defaults(void)
{
    /* GPIO date + matrix reset defaults */
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_DATE)), ==, ESP32S3_GPIO_DATE_VERSION);
    g_assert_cmphex(qtest_readl(s, FUNC_IN(0)), ==, GPIO_FUNC_IN_LOW);
    g_assert_cmphex(qtest_readl(s, FUNC_IN(255)), ==, GPIO_FUNC_IN_LOW);
    g_assert_cmphex(qtest_readl(s, FUNC_OUT(0)), ==, GPIO_FUNC_OUT_SEL_NONE);
    g_assert_cmphex(qtest_readl(s, PIN_REG(0)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_OUT)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_ENABLE)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, 0);

    /* IO_MUX: FUN_DRV = 2 reset default, version register */
    g_assert_cmphex(qtest_readl(s, IO_MUX_PAD(0)), ==, ESP32S3_IOMUX_GPIO_REG_DEFAULT);
    g_assert_cmphex(qtest_readl(s, IO_MUX_PAD(48)), ==, ESP32S3_IOMUX_GPIO_REG_DEFAULT);
    g_assert_cmphex(qtest_readl(s, IOMUX(A_IO_MUX_DATE)), ==, ESP32S3_IOMUX_DATE_VERSION);

    /* RTC_IO pad config reset default: DRV = 2, RDE = 1 */
    g_assert_cmphex(qtest_readl(s, RTC_PAD_CFG(0)), ==, ESP32S3_RTC_IO_PAD_DEFAULT);
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_DATE_OFF)), ==,
                    ESP32S3_RTC_IO_DATE_VERSION);
}

static void test_w1ts_w1tc(void)
{
    /* Output enable / data with W1TS/W1TC */
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_ENABLE)), ==, BIT(5));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_OUT)), ==, BIT(5));
    /* W1TS/W1TC registers read as zero */
    g_assert_cmpuint(qtest_readl(s, GPIO(A_GPIO_OUT_W1TS)), ==, 0);
    g_assert_cmpuint(qtest_readl(s, GPIO(A_GPIO_OUT_W1TC)), ==, 0);
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_OUT)), ==, 0);
    /* Direct OUT write sets the whole register */
    qtest_writel(s, GPIO(A_GPIO_OUT), 0x12340000);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_OUT)), ==, 0x12340000);
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), 0xFFFFFFFF);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TC), 0xFFFFFFFF);

    /* The "1" banks are 22 bits wide (GPIO32-48) */
    qtest_writel(s, GPIO(A_GPIO_OUT1), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_OUT1)), ==, ESP32S3_GPIO1_MASK);
    qtest_writel(s, GPIO(A_GPIO_ENABLE1_W1TS), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_ENABLE1)), ==, ESP32S3_GPIO1_MASK);
    qtest_writel(s, GPIO(A_GPIO_ENABLE1_W1TC), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_ENABLE1)), ==, 0);
    qtest_writel(s, GPIO(A_GPIO_OUT1_W1TC), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_OUT1)), ==, 0);
}

static void test_input_readonly(void)
{
    uint32_t before = qtest_readl(s, GPIO(A_GPIO_IN));

    qtest_writel(s, GPIO(A_GPIO_IN), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, before);
    qtest_writel(s, GPIO(A_GPIO_IN1), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN1)), ==, 0);
}

static void test_external_input(void)
{
    /* FUN_IE on pad 4, matrix function: external level reaches GPIO_IN */
    qtest_writel(s, IO_MUX_PAD(4), IOMUX_GPIO_IN_EN);
    g_assert_cmpuint(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    drive_input(4, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(4));
    drive_input(4, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* Without FUN_IE the input buffer is disabled: samples 0 */
    qtest_writel(s, IO_MUX_PAD(4), 0);
    drive_input(4, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);
    drive_input(4, 0);
    qtest_writel(s, IO_MUX_PAD(4), 0);

    /* High bank: pad 33 via GPIO_IN1 */
    qtest_writel(s, IO_MUX_PAD(33), IOMUX_GPIO_IN_EN);
    drive_input(33, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN1)), ==, BIT(33 - 32));
    drive_input(33, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN1)), ==, 0);
    qtest_writel(s, IO_MUX_PAD(33), 0);
}

static void test_simple_gpio_loopback(void)
{
    /* Pad 2: output enabled, drives the net, input loopback reads it */
    qtest_writel(s, IO_MUX_PAD(2), IOMUX_GPIO_IN_EN);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(2));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(2));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    /* Open drain: output 1 releases the pad; no external driver, no pull ->
     * the net is floating (unresolved, sampled 0), never a valid low */
    qtest_writel(s, PIN_REG(2), R_GPIO_PINn_PAD_DRIVER_MASK);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);
    /* The weak pull-up resolves the released open-drain net to high */
    qtest_writel(s, IO_MUX_PAD(2), IOMUX_GPIO_IN_EN | R_IO_MUX_GPIOn_FUN_PU_MASK);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));
    /* An external low driver beats the weak pull */
    drive_input(2, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);
    drive_input(2, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    /* Cleanup: push-pull, disabled, external detached */
    qtest_writel(s, PIN_REG(2), 0);
    qtest_writel(s, IO_MUX_PAD(2), 0);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TC), BIT(2));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(2));
    drive_input(2, 0);
}

static void test_matrix_out_routing(void)
{
    qtest_writel(s, IO_MUX_PAD(2), IOMUX_GPIO_IN_EN);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(2));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(2));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    /* Route the pad to a peripheral output signal (1): no model sources it,
     * the pad driver releases and the floating net samples 0 */
    qtest_writel(s, FUNC_OUT(2), 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* Reserved selector 511: still no drive */
    qtest_writel(s, FUNC_OUT(2), 0x1FF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* Simple GPIO (256) drives again */
    qtest_writel(s, FUNC_OUT(2), GPIO_FUNC_OUT_SEL_NONE);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    /* Simple GPIO (256): OE is hardwired to GPIO_ENABLE, OEN_SEL does
     * not apply there (IDF gpio_config use-case) */
    qtest_writel(s, FUNC_OUT(2), GPIO_FUNC_OUT_SEL_NONE | R_GPIO_FUNCn_OUT_SEL_CFG_FUNC_OEN_SEL_MASK);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    /* Output inversion: OUT bit 0 but inverted -> drives 1 */
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(2));
    qtest_writel(s, FUNC_OUT(2), GPIO_FUNC_OUT_SEL_NONE | R_GPIO_FUNCn_OUT_SEL_CFG_FUNC_OUT_INV_SEL_MASK);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    /* OE inversion on the simple path: enable registered but inverted ->
     * released */
    qtest_writel(s, FUNC_OUT(2), GPIO_FUNC_OUT_SEL_NONE |
                 R_GPIO_FUNCn_OUT_SEL_CFG_FUNC_OUT_INV_SEL_MASK |
                 R_GPIO_FUNCn_OUT_SEL_CFG_FUNC_OEN_INV_SEL_MASK);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* Normal peripheral-OE route (IDF matrix use-case): route a real
     * peripheral signal (I2CEXT0_SDA = 90) with OEN_SEL = 0 so OE comes
     * from the peripheral signal while GPIO_ENABLE is unset.  No model
     * sources the signal, so the pad stays released and the net floats
     * (samples 0 under the permissive profile). */
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TC), BIT(2));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(2));
    qtest_writel(s, FUNC_OUT(2), 90);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* Same peripheral route with OEN_SEL = 1: OE from GPIO_ENABLE (still
     * unset) -> released */
    qtest_writel(s, FUNC_OUT(2), 90 | R_GPIO_FUNCn_OUT_SEL_CFG_FUNC_OEN_SEL_MASK);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* OEN_SEL = 1 with GPIO_ENABLE set: pad drives the (unmodelled)
     * peripheral signal value 0 -> reads 0 (value-source boundary) */
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(2));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* Back to simple GPIO 256 with the inverted source: drives 1 again
     * (GPIO_ENABLE still set, OEN_SEL irrelevant on the simple path) */
    qtest_writel(s, FUNC_OUT(2), GPIO_FUNC_OUT_SEL_NONE |
                 R_GPIO_FUNCn_OUT_SEL_CFG_FUNC_OUT_INV_SEL_MASK);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    /* Cleanup */
    qtest_writel(s, FUNC_OUT(2), GPIO_FUNC_OUT_SEL_NONE);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TC), BIT(2));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(2));
    qtest_writel(s, IO_MUX_PAD(2), 0);
    drive_input(2, 0);
}

static void test_matrix_in_routing(void)
{
    /* Register semantics of the 256 input selectors */
    qtest_writel(s, FUNC_IN(3), 5 | R_GPIO_FUNCn_IN_SEL_CFG_FUNC_IN_INV_SEL_MASK |
                 R_GPIO_FUNCn_IN_SEL_CFG_SIG_IN_SEL_MASK);
    g_assert_cmphex(qtest_readl(s, FUNC_IN(3)), ==,
                    5 | R_GPIO_FUNCn_IN_SEL_CFG_FUNC_IN_INV_SEL_MASK |
                    R_GPIO_FUNCn_IN_SEL_CFG_SIG_IN_SEL_MASK);
    /* Only the low byte is implemented */
    qtest_writel(s, FUNC_IN(3), 0xFFFFFFC1);
    g_assert_cmphex(qtest_readl(s, FUNC_IN(3)), ==, 0xC1);
    /* Constant-input selectors store as-is */
    qtest_writel(s, FUNC_IN(4), GPIO_FUNC_IN_HIGH);
    g_assert_cmphex(qtest_readl(s, FUNC_IN(4)), ==, GPIO_FUNC_IN_HIGH);
    qtest_writel(s, FUNC_IN(4), GPIO_FUNC_IN_LOW);
    g_assert_cmphex(qtest_readl(s, FUNC_IN(4)), ==, GPIO_FUNC_IN_LOW);
}

static void test_pin_interrupts(void)
{
    uint32_t pin5 = PIN_REG(5);

    qtest_writel(s, IO_MUX_PAD(5), IOMUX_GPIO_IN_EN);
    drive_input(5, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, 0);

    /* Rising edge on pad 5, enabled for the PROCPU lane (INT_ENA bit 0) */
    qtest_writel(s, pin5, (ESP32S3_GPIO_INT_RISING << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_PROCPU);
    drive_input(5, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT)), ==, BIT(5));
    /* Not enabled for the NMI lane, and pad 5 is in the low bank */
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_NMI_INT)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT1)), ==, 0);

    /* W1TC clears the status; a rising edge already consumed does not
     * re-assert the edge interrupt */
    qtest_writel(s, GPIO(A_GPIO_STATUS_W1TC), BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT)), ==, 0);

    /* Falling edge */
    qtest_writel(s, pin5, (ESP32S3_GPIO_INT_FALLING << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_PROCPU);
    drive_input(5, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));
    qtest_writel(s, GPIO(A_GPIO_STATUS_W1TC), BIT(5));

    /* Any edge */
    qtest_writel(s, pin5, (ESP32S3_GPIO_INT_ANYEDGE << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_PROCPU);
    drive_input(5, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));
    qtest_writel(s, GPIO(A_GPIO_STATUS_W1TC), BIT(5));
    drive_input(5, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));
    qtest_writel(s, GPIO(A_GPIO_STATUS_W1TC), BIT(5));

    /* High level: the status re-asserts while the condition persists —
     * clearing cannot stick until the level goes away (TRM level semantics) */
    qtest_writel(s, pin5, (ESP32S3_GPIO_INT_HIGH << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_PROCPU);
    drive_input(5, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));
    qtest_writel(s, GPIO(A_GPIO_STATUS_W1TC), BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));
    drive_input(5, 1); /* a fresh evaluation re-asserts the level */
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));

    /* STATUS_NEXT mirrors the pending level condition */
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS_NEXT)), ==, BIT(5));

    /* APPCPU lane: INT_ENA bit 1 raises matrix source 18; the PROCPU
     * masked views stay clear (there is no APPCPU status register) */
    qtest_writel(s, pin5, (ESP32S3_GPIO_INT_HIGH << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_APPCPU);
    drive_input(5, 0);
    drive_input(5, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT1)), ==, 0);

    /* PROCPU NMI lane: INT_ENA bit 2 feeds PCPU_NMI_INT */
    qtest_writel(s, pin5, (ESP32S3_GPIO_INT_HIGH << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_PROCPU_NMI);
    drive_input(5, 0);
    drive_input(5, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_NMI_INT)), ==, BIT(5));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT)), ==, 0);

    /* High bank: pad 33 rising edge enabled for PROCPU shows up in
     * PCPU_INT1, not in PCPU_INT */
    qtest_writel(s, IO_MUX_PAD(33), IOMUX_GPIO_IN_EN);
    qtest_writel(s, PIN_REG(33), (ESP32S3_GPIO_INT_RISING << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_PROCPU);
    drive_input(33, 1);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS1)), ==, BIT(33 - 32));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT1)), ==, BIT(33 - 32));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_PCPU_INT)), ==, 0);

    /* Disabled interrupt: no status update */
    qtest_writel(s, pin5, 0);
    qtest_writel(s, PIN_REG(33), 0);
    qtest_writel(s, GPIO(A_GPIO_STATUS_W1TC), BIT(5));
    qtest_writel(s, GPIO(A_GPIO_STATUS1_W1TC), BIT(33 - 32));
    drive_input(5, 0);
    drive_input(33, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS1)), ==, 0);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STATUS_NEXT)), ==, 0);

    qtest_writel(s, IO_MUX_PAD(5), 0);
    qtest_writel(s, IO_MUX_PAD(33), 0);
    drive_input(5, 0);
    drive_input(33, 0);
}

#ifdef GPIO_TEST_HAS_MATRIX_DELIVERY
/*
 * End-to-end GPIO -> interrupt matrix -> CPU line proof on the current
 * intmatrix model: route source 16 to CPU int 8 (what the real
 * esp_intr_alloc writes), drive a rising edge on pad 5 and observe
 * INTR_STATUS_0 bit 16 and the asserted CPU output line 6
 * (arch int 8 = external input 6 on the esp32s3 core).
 * Constants mirror include/hw/xtensa/esp32s3_intc.h (not included here:
 * it is target-specific and qtests compile target-independently).
 */
#define GPIO_TEST_INTMATRIX_STATUS_OFFSET 0x18C /* INTR_STATUS_0 */
#define GPIO_TEST_INTMATRIX_OUT_NAME "cpu-int-out"
static void test_gpio_matrix_irq_delivery(void)
{
    const uint64_t INTMATRIX_BASE = 0x600C2000ULL;
    const int cpu_int_line = 6; /* arch int 8 = external input 6 */

    qtest_writel(s, IO_MUX_PAD(5), IOMUX_GPIO_IN_EN);
    drive_input(5, 0);

    /* Map source 16 -> CPU interrupt 8, exactly like esp_intr_alloc */
    qtest_writel(s, INTMATRIX_BASE + 16 * 4, 8);

    /* Intercept the matrix CPU output lines so the assertion of the
     * arch-int-8 line is observable (this detaches them from the CPU for
     * the remainder of the test, hence it runs last). */
    qtest_irq_intercept_out_named(s, "/machine/soc/intmatrix",
                                  GPIO_TEST_INTMATRIX_OUT_NAME);

    /* Arm the rising edge on pad 5 and drive it */
    qtest_writel(s, PIN_REG(5), (ESP32S3_GPIO_INT_RISING << R_GPIO_PINn_INT_TYPE_SHIFT) |
                 ESP32S3_GPIO_INT_ENA_PROCPU);
    drive_input(5, 1);

    /* The matrix saw the source: INTR_STATUS_0 bit 16 (cpu0 file) */
    g_assert_cmphex(qtest_readl(s, INTMATRIX_BASE + GPIO_TEST_INTMATRIX_STATUS_OFFSET) & BIT(16),
                    ==, BIT(16));

    /* And the CPU int line 6 asserted */
    g_assert_cmpint(qtest_get_irq(s, cpu_int_line), ==, 1);

    qtest_writel(s, GPIO(A_GPIO_STATUS_W1TC), BIT(5));
    drive_input(5, 0);
    g_assert_cmpint(qtest_get_irq(s, cpu_int_line), ==, 0);
}
#endif

static void test_iomux_disconnect(void)
{
    /* With MCU_SEL != PIN_FUNC_GPIO the pad selects a direct IO_MUX
     * function: the simple GPIO output no longer drives the net */
    qtest_writel(s, IO_MUX_PAD(2), IOMUX_GPIO_IN_EN);
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TS), BIT(2));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TS), BIT(2));
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, BIT(2));

    qtest_writel(s, IO_MUX_PAD(2), R_IO_MUX_GPIOn_FUN_IE_MASK); /* MCU_SEL = 0 */
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_IN)), ==, 0);

    /* Pad register write mask: only bits [15:0] are implemented */
    qtest_writel(s, IO_MUX_PAD(2), 0x12345678);
    g_assert_cmphex(qtest_readl(s, IO_MUX_PAD(2)), ==, 0x5678);

    /* PIN_CTRL stores bits [11:0] */
    qtest_writel(s, IOMUX(A_IO_MUX_PIN_CTRL), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, IOMUX(A_IO_MUX_PIN_CTRL)), ==, 0xFFF);

    /* Out-of-range IO_MUX register: the 0xC8..0xFB gap between the last
     * pad register (GPIO48 at 0xC4) and DATE reads 0 and ignores writes */
    qtest_writel(s, IOMUX(0xC8), 0xFFFFFFFF);
    g_assert_cmpuint(qtest_readl(s, IOMUX(0xC8)), ==, 0);

    /* Cleanup */
    qtest_writel(s, GPIO(A_GPIO_ENABLE_W1TC), BIT(2));
    qtest_writel(s, GPIO(A_GPIO_OUT_W1TC), BIT(2));
    qtest_writel(s, IO_MUX_PAD(2), ESP32S3_IOMUX_GPIO_REG_DEFAULT);
    drive_input(2, 0);
}

static void test_negative(void)
{
    /* GPIO_STRAP is read-only */
    uint32_t strap = qtest_readl(s, GPIO(A_GPIO_STRAP));
    qtest_writel(s, GPIO(A_GPIO_STRAP), 0xFFFF);
    g_assert_cmphex(qtest_readl(s, GPIO(A_GPIO_STRAP)), ==, strap);

    /* Status registers and read-only views ignore writes */
    qtest_writel(s, GPIO(A_GPIO_STATUS_NEXT), 0xFFFFFFFF);
    g_assert_cmpuint(qtest_readl(s, GPIO(A_GPIO_STATUS_NEXT)), ==, 0);
    qtest_writel(s, GPIO(A_GPIO_PCPU_INT), 0xFFFFFFFF);
    g_assert_cmpuint(qtest_readl(s, GPIO(A_GPIO_PCPU_INT)), ==, 0);

    /* Reserved hole between the pin file and the matrix (pads 49..53 and
     * the STATUS_NEXT gap at 0x138..0x14B read 0 and ignore writes) */
    qtest_writel(s, GPIO(0x138), 0xFFFFFFFF);
    g_assert_cmpuint(qtest_readl(s, GPIO(0x138)), ==, 0);

    /* Reserved region between CLOCK_GATE and DATE */
    qtest_writel(s, GPIO(0x640), 0xFFFFFFFF);
    g_assert_cmpuint(qtest_readl(s, GPIO(0x640)), ==, 0);

    /* RTC_IO reserved hole after TOUCH_CTRL */
    qtest_writel(s, RTCIO(0xEC), 0xFFFFFFFF);
    g_assert_cmpuint(qtest_readl(s, RTCIO(0xEC)), ==, 0);

    /* RTC_IO version register ignores writes */
    qtest_writel(s, RTCIO(RTC_IO_DATE_OFF), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_DATE_OFF)), ==,
                    ESP32S3_RTC_IO_DATE_VERSION);
}

static void test_rtc_io(void)
{
    /* RTC data registers live in bits [31:10] */
    qtest_writel(s, RTCIO(RTC_IO_OUT_W1TS_OFF), 1u << (10 + 0));
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_OUT_OFF)), ==, 1u << 10);
    qtest_writel(s, RTCIO(RTC_IO_ENABLE_W1TS_OFF), 1u << (10 + 0));
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_ENABLE_OFF)), ==, 1u << 10);
    /* W1TC registers read as zero */
    g_assert_cmpuint(qtest_readl(s, RTCIO(RTC_IO_OUT_W1TS_OFF)), ==, 0);
    g_assert_cmpuint(qtest_readl(s, RTCIO(RTC_IO_ENABLE_W1TC_OFF)), ==, 0);

    /* Direct write only touches bits [31:10] */
    qtest_writel(s, RTCIO(RTC_IO_OUT_OFF), 0xFFFFFFFF);
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_OUT_OFF)), ==,
                    ESP32S3_RTC_IO_DATA_MASK);

    /* Input pad 3 with rising edge interrupt; FUN_IE (bit 13) gates the
     * input buffer for both RTC_GPIO_IN and the interrupt detector */
    qtest_writel(s, RTC_PAD_CFG(3), (1u << 13)); /* FUN_IE */
    qtest_writel(s, RTCIO(RTC_IO_PIN_OFF(3)), (1 << 7)); /* rising */
    drive_rtc_input(3, 1);
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_STATUS_OFF)), ==, 1u << (10 + 3));
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_IN_OFF)), ==, 1u << (10 + 3));
    qtest_writel(s, RTCIO(RTC_IO_STATUS_W1TC_OFF), 1u << (10 + 3));
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_STATUS_OFF)), ==, 0);

    /* Pull-down (RDE) resolves the released pad; the external driver wins */
    qtest_writel(s, RTC_PAD_CFG(3), (1u << 13) | (1u << 28)); /* FUN_IE | RDE */
    drive_rtc_input(3, 1);
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_IN_OFF)), ==, 1u << (10 + 3));
    drive_rtc_input(3, 0);
    g_assert_cmphex(qtest_readl(s, RTCIO(RTC_IO_IN_OFF)), ==, 0);

    /* Out-of-range RTC pad config read: gap between pin file and DEBUG_SEL
     * is the 0x80 DEBUG_SEL register; 0xD9..0xDB inside the pad map does
     * not exist — the next word is EXT_WAKEUP0 which we do not disturb.
     * Check the unmapped hole instead (0xEC is covered above). */

    /* Cleanup */
    qtest_writel(s, RTC_PAD_CFG(3), ESP32S3_RTC_IO_PAD_DEFAULT);
    qtest_writel(s, RTCIO(RTC_IO_ENABLE_W1TC_OFF), ESP32S3_RTC_IO_DATA_MASK);
    qtest_writel(s, RTCIO(RTC_IO_OUT_W1TC_OFF), ESP32S3_RTC_IO_DATA_MASK);
    qtest_writel(s, RTCIO(RTC_IO_PIN_OFF(3)), 0);
    drive_rtc_input(3, 0);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    s = qtest_initf("-machine esp32s3 -L pc-bios "
                    "-global driver=esp32s3.gpio,property=strap_mode,value=0x00");

    qtest_add_func("esp32s3-gpio/reset-defaults", test_reset_defaults);
    qtest_add_func("esp32s3-gpio/w1ts-w1tc", test_w1ts_w1tc);
    qtest_add_func("esp32s3-gpio/input-readonly", test_input_readonly);
    qtest_add_func("esp32s3-gpio/external-input", test_external_input);
    qtest_add_func("esp32s3-gpio/simple-loopback", test_simple_gpio_loopback);
    qtest_add_func("esp32s3-gpio/matrix-out", test_matrix_out_routing);
    qtest_add_func("esp32s3-gpio/matrix-in", test_matrix_in_routing);
    qtest_add_func("esp32s3-gpio/pin-interrupts", test_pin_interrupts);
    qtest_add_func("esp32s3-gpio/iomux-disconnect", test_iomux_disconnect);
    qtest_add_func("esp32s3-gpio/negative", test_negative);
    qtest_add_func("esp32s3-gpio/rtc-io", test_rtc_io);
#ifdef GPIO_TEST_HAS_MATRIX_DELIVERY
    qtest_add_func("esp32s3-gpio/matrix-irq-delivery", test_gpio_matrix_irq_delivery);
#endif

    ret = g_test_run();
    qtest_quit(s);
    return ret;
}
