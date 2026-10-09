// SPDX-License-Identifier: MIT
#include <Arduino.h>
#include <cstring>

// Ordinary HardwareSerial only. No driver/MMIO helper, internal loopback,
// console RX injection, external echo device, or generated RX substitute.
static constexpr size_t count = 513;
static unsigned failures;
static uint8_t tx1[count], tx2[count], rx1[count], rx2[count], tx0[count];
static uint8_t loop_received[3][count];
static const unsigned long bauds[] = {9600, 115200, 921600};

static void check(const char *name, bool ok)
{
    failures += !ok;
    Serial0.printf("ARDUINO_UART_CHECK name=%s result=%s\n", name, ok ? "PASS" : "FAIL");
}

static void bytes(const char *name, const uint8_t *data, size_t size)
{
    Serial0.printf("ARDUINO_UART_BYTES name=%s size=%u data=", name, static_cast<unsigned>(size));
    for (size_t i = 0; i < size; ++i) { Serial0.printf("%02x", data[i]); }
    Serial0.println();
}

static void duplex(const char *name, unsigned long baud, unsigned salt, uint32_t format, unsigned mask)
{
    Serial0.flush(true);
    Serial1.end();
    Serial2.end();
    // end() detaches TX to the GPIO matrix but leaves its output enabled.
    // Release both unused pads before either receiver is installed: the real
    // project pull-ups, not a GPIO-driven substitute, establish UART idle.
    pinMode(17, INPUT);
    pinMode(18, INPUT);
    bool idle1 = digitalRead(17) == HIGH;
    bool idle2 = digitalRead(18) == HIGH;
    Serial0.printf("ARDUINO_UART_IDLE name=%s tx1=%u tx2=%u\n", name,
        static_cast<unsigned>(idle1), static_cast<unsigned>(idle2));
    check("tx1_released_idle", idle1);
    check("tx2_released_idle", idle2);
    check("rx_buffer1", Serial1.setRxBufferSize(4096) == 4096);
    check("tx_buffer1", Serial1.setTxBufferSize(4096) == 4096);
    check("rx_buffer2", Serial2.setRxBufferSize(4096) == 4096);
    check("tx_buffer2", Serial2.setTxBufferSize(4096) == 4096);
    check("clock1", Serial1.setClockSource(UART_CLK_SRC_XTAL));
    check("clock2", Serial2.setClockSource(UART_CLK_SRC_XTAL));
    // HardwareSerial begin() can log through IDF's console. Drain our report
    // stream before those writes, rather than interleaving two TX producers.
    Serial0.flush(true);
    Serial1.begin(baud, format, 15, 17, false, 20000UL, 32);
    Serial0.flush(true);
    Serial2.begin(baud, format, 16, 18, false, 20000UL, 32);
    Serial0.flush(true);
    Serial1.setTimeout(2500);
    Serial2.setTimeout(2500);
    check("begin1", static_cast<bool>(Serial1));
    check("begin2", static_cast<bool>(Serial2));
    for (size_t i = 0; i < count; ++i) {
        tx1[i] = (i * 37 + salt + 3) & mask;
        tx2[i] = (i * 53 + salt + 91) & mask;
    }
    unsigned long start = micros();
    size_t sent1 = Serial1.write(tx1, count);
    size_t sent2 = Serial2.write(tx2, count);
    size_t got1 = Serial1.readBytes(rx1, count);
    size_t got2 = Serial2.readBytes(rx2, count);
    Serial1.flush(true);
    Serial2.flush(true);
    unsigned long elapsed = micros() - start;
    bool exact = sent1 == count && sent2 == count && got1 == count && got2 == count &&
        !memcmp(tx2, rx1, count) && !memcmp(tx1, rx2, count);
    Serial0.printf("ARDUINO_UART_TRANSFER name=%s baud=%lu salt=%u mask=%u n1=%u n2=%u elapsed_us=%lu actual_baud1=%lu actual_baud2=%lu\n",
        name, baud, salt, mask, static_cast<unsigned>(got1), static_cast<unsigned>(got2), elapsed,
        static_cast<unsigned long>(Serial1.baudRate()), static_cast<unsigned long>(Serial2.baudRate()));
    check(name, exact);
    if (got1 > 0) { bytes("rx1", rx1, got1); }
    if (got2 > 0) { bytes("rx2", rx2, got2); }
    Serial0.flush(true);
}

static void uart0_loopbacks()
{
    struct result_t {
        bool buffers, clock, installed, exact;
        size_t sent, received;
        unsigned long elapsed, actual_baud;
    } result[3] = {};
    Serial0.flush(true);
    Serial0.end();
    // During physical UART0 tests, no logging occurs on its GPIO4/5 net.
    // Keep each actual RX stream until the ordinary console routing returns.
    for (unsigned test = 0; test < 3; ++test) {
        auto &r = result[test];
        r.buffers = Serial0.setRxBufferSize(4096) == 4096 && Serial0.setTxBufferSize(4096) == 4096;
        r.clock = Serial0.setClockSource(UART_CLK_SRC_XTAL);
        Serial0.begin(bauds[test], SERIAL_8N1, 5, 4, false, 20000UL, 32);
        Serial0.setTimeout(2500);
        r.installed = static_cast<bool>(Serial0);
        for (size_t i = 0; i < count; ++i) { tx0[i] = (i * 61 + test * 17 + 43) & 255; }
        unsigned long start = micros();
        r.sent = Serial0.write(tx0, count);
        r.received = Serial0.readBytes(loop_received[test], count);
        Serial0.flush(true);
        r.elapsed = micros() - start;
        r.actual_baud = Serial0.baudRate();
        r.exact = r.sent == count && r.received == count && !memcmp(tx0, loop_received[test], count);
        Serial0.end();
    }
    bool console_buffers = Serial0.setRxBufferSize(4096) == 4096 && Serial0.setTxBufferSize(4096) == 4096;
    bool console_clock = Serial0.setClockSource(UART_CLK_SRC_XTAL);
    Serial0.begin(115200, SERIAL_8N1, 44, 43);
    Serial0.println(); // Delimit the independent chardev's binary TX mirror.
    check("uart0_console_restore", console_buffers && console_clock && static_cast<bool>(Serial0));
    for (unsigned test = 0; test < 3; ++test) {
        auto &r = result[test];
        const char *name = test == 0 ? "uart0_loop9600" : test == 1 ? "uart0_loop115200" : "uart0_loop921600";
        Serial0.printf("ARDUINO_UART_LOOPBACK name=%s baud=%lu salt=%u sent=%u received=%u elapsed_us=%lu actual_baud=%lu\n",
            name, bauds[test], test * 17, static_cast<unsigned>(r.sent), static_cast<unsigned>(r.received), r.elapsed, r.actual_baud);
        check(name, r.buffers && r.clock && r.installed && r.exact);
        if (r.received > 0) { bytes("uart0_rx", loop_received[test], r.received); }
    }
    Serial0.println("ARDUINO_UART_SCOPE external_uart0_tx=4 external_uart0_rx=5 console_tx=43 console_rx=44 console_rx_injection=never");
}

void setup()
{
    Serial0.setRxBufferSize(4096);
    Serial0.setTxBufferSize(4096);
    Serial0.setClockSource(UART_CLK_SRC_XTAL);
    Serial0.begin(115200, SERIAL_8N1, 44, 43);
    // UART0 installation may reset queued startup logs mid-line; delimit the
    // first fixture record on the newly installed HardwareSerial stream.
    Serial0.println();
    Serial0.printf("ARDUINO_UART_BOOT profile=arduino arduino=%s idf=%s\n", ESP_ARDUINO_VERSION_STR, ESP.getSdkVersion());
    Serial0.println("ARDUINO_UART_PINS tx1=17 rx1=15 tx2=18 rx2=16 uart0_tx=4 uart0_rx=5 console_tx=43 console_rx=44");
    duplex("duplex9600", 9600, 1, SERIAL_8N1, 255);
    duplex("duplex115200", 115200, 7, SERIAL_8N1, 255);
    duplex("duplex921600", 921600, 13, SERIAL_8N1, 255);
    duplex("width7_even_stop2", 115200, 23, SERIAL_7E2, 127);
    duplex("width8_odd_stop2", 115200, 31, SERIAL_8O2, 255);
    Serial1.end();
    Serial2.end();
    check("end1", !static_cast<bool>(Serial1));
    check("end2", !static_cast<bool>(Serial2));
    uart0_loopbacks();
    Serial0.println("ARDUINO_UART_PROFILE arduino_commit=94afccf35fb1e401facddbcf9e13bcf7c76a31d8 idf_commit=b774170ff46c393eeb5e495ea37936038d3f4f4f idf61_qualification=not_claimed");
    Serial0.printf("ARDUINO_UART_DONE profile=arduino failures=%u result=%s\n", failures, failures ? "FAIL" : "PASS");
    Serial0.flush(true);
}

void loop()
{
    delay(1000);
}
