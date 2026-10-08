/* SPDX-License-Identifier: GPL-2.0-or-later
 * Include in hw/xtensa/esp32s3.c after esp32s3_uart.h + esp32s3_uhci.h.
 * Call s3_uhci_bind_uart once for each real UART after both devices realize.
 */
static bool s3_uhci_uart_write(void *opaque, uint8_t byte)
{
    return esp32s3_uart_dma_write(opaque, byte);
}
static bool s3_uhci_uart_read(void *opaque, uint8_t *byte)
{
    return esp32s3_uart_dma_read(opaque, byte);
}
static unsigned s3_uhci_uart_free(void *opaque)
{
    return esp32s3_uart_tx_free(opaque);
}
static unsigned s3_uhci_uart_used(void *opaque)
{
    return esp32s3_uart_rx_used(opaque);
}
static void s3_uhci_uart_event(void *opaque, unsigned index, bool is_break,
                              bool delimiter_queued, unsigned queued_bytes)
{
    esp32s3_uhci_rx_event(opaque, index,
                         is_break ? S3_UHCI_RX_BREAK : S3_UHCI_RX_IDLE,
                         delimiter_queued, queued_bytes);
}
static void s3_uhci_bind_uart(ESP32S3UhciState *uhci,
                              ESP32S3UARTState *uart, unsigned index)
{
    const S3UhciUart ops = { uart, s3_uhci_uart_write, s3_uhci_uart_read,
                           s3_uhci_uart_free, s3_uhci_uart_used };
    esp32s3_uhci_attach_uart(uhci, index, &ops);
    esp32s3_uart_set_dma_notify(uart, esp32s3_uhci_kick, uhci);
    esp32s3_uart_set_rx_event(uart, s3_uhci_uart_event, uhci);
}
