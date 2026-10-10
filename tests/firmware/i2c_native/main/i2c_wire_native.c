/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_I2C_NATIVE_PROFILE_WIRE_DEFINED
static const char *profile = "wire_defined";
#else
static const char *profile = "wire";
#endif

static unsigned failures;
static void check(unsigned master, unsigned ten, const char *name, bool ok)
{
    printf("I2C_WIRE_CHECK master=%u ten=%u name=%s result=%s\n", master, ten, name, ok ? "PASS" : "FAIL");
    failures += !ok;
    fflush(stdout);
}
typedef struct {
    i2c_slave_dev_handle_t handle;
    TaskHandle_t task;
    portMUX_TYPE lock;
    uint8_t received[128], response[65];
    unsigned length, callbacks, requests, served;
    bool overflow, quit, exited;
} Peer;
static bool on_receive(i2c_slave_dev_handle_t handle,
                       const i2c_slave_rx_done_event_data_t *event, void *opaque)
{
    Peer *p = opaque;
    (void)handle;
    portENTER_CRITICAL_ISR(&p->lock);
    if (event->length > sizeof(p->received) - p->length) p->overflow = true;
    else { memcpy(p->received + p->length, event->buffer, event->length); p->length += event->length; }
    p->callbacks++;
    portEXIT_CRITICAL_ISR(&p->lock);
    return false;
}
static bool on_request(i2c_slave_dev_handle_t handle,
                       const i2c_slave_request_event_data_t *event, void *opaque)
{
    Peer *p = opaque;
    (void)handle; (void)event;
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&p->lock); p->requests++; portEXIT_CRITICAL_ISR(&p->lock);
    vTaskNotifyGiveFromISR(p->task, &woken);
    return woken == pdTRUE;
}
static void responder(void *opaque)
{
    Peer *p = opaque;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        portENTER_CRITICAL(&p->lock);
        unsigned requests = p->requests;
        bool quit = p->quit;
        portEXIT_CRITICAL(&p->lock);
        if (quit) break;
        if (requests > p->served) {
            uint32_t written = 0;
            unsigned length = requests == 1 ? 65 : 17;
            esp_err_t err = i2c_slave_write(p->handle, p->response, length, &written, 500);
            portENTER_CRITICAL(&p->lock);
            p->overflow |= err != ESP_OK || written != length;
            p->served = requests;
            portEXIT_CRITICAL(&p->lock);
        }
    }
    portENTER_CRITICAL(&p->lock);
    p->exited = true;
    portEXIT_CRITICAL(&p->lock);
    /* The device owns this task handle until callbacks are unregistered. */
    vTaskSuspend(NULL);
}
static bool run(unsigned master, unsigned ten)
{
    const int sda[2] = {CONFIG_I2C_NATIVE_SDA0, CONFIG_I2C_NATIVE_SDA1};
    const int scl[2] = {CONFIG_I2C_NATIVE_SCL0, CONFIG_I2C_NATIVE_SCL1};
    static Peer peer = {.lock = portMUX_INITIALIZER_UNLOCKED};
    peer.handle = NULL;
    peer.task = NULL;
    peer.length = peer.callbacks = peer.requests = peer.served = 0;
    peer.overflow = peer.quit = peer.exited = false;
    bool released = true;
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL, defined = NULL;
    unsigned slave = 1 - master, address = ten ? 0x1a2 : 0x42;
    i2c_addr_bit_len_t bits = ten ? I2C_ADDR_BIT_LEN_10 : I2C_ADDR_BIT_LEN_7;
    i2c_slave_config_t sc = {.i2c_port = slave, .sda_io_num = sda[slave], .scl_io_num = scl[slave],
        .clk_source = I2C_CLK_SRC_DEFAULT, .send_buf_depth = 256, .receive_buf_depth = 256,
        .slave_addr = address, .addr_bit_len = bits, .flags.enable_internal_pullup = false};
    bool ok = i2c_new_slave_device(&sc, &peer.handle) == ESP_OK;
    check(master, ten, "new_slave", ok);
    if (!ok) return true;
    BaseType_t created = xTaskCreate(responder, "wire-response", 4096, &peer, 6, &peer.task);
    check(master, ten, "responder_task", created == pdPASS);
    if (created != pdPASS) goto done;
    i2c_slave_event_callbacks_t cb = {.on_receive = on_receive, .on_request = on_request};
    ok = i2c_slave_register_event_callbacks(peer.handle, &cb, &peer) == ESP_OK;
    check(master, ten, "callbacks", ok); if (!ok) goto done;
    i2c_master_bus_config_t mc = {.i2c_port = master, .sda_io_num = sda[master], .scl_io_num = scl[master],
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = false};
    ok = i2c_new_master_bus(&mc, &bus) == ESP_OK;
    check(master, ten, "new_master", ok); if (!ok) goto done;
    i2c_device_config_t dc = {.dev_addr_length = bits, .device_address = address,
        .scl_speed_hz = CONFIG_I2C_NATIVE_SPEED_HZ};
    ok = i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK;
    check(master, ten, "add_device", ok); if (!ok) goto done;
    /* Retain buffers if public device/bus teardown fails with work outstanding. */
    static uint8_t sent[65], actual[65];
    for (unsigned i = 0; i < 65; i++) {
        sent[i] = (i * 7 + master * 31 + ten * 19 + 3) & 255;
        peer.response[i] = (i * 11 + master * 17 + ten * 23 + 0xa0) & 255;
    }
    check(master, ten, "write65", i2c_master_transmit(dev, sent, 65, 1000) == ESP_OK);
    memset(actual, 0, sizeof(actual));
    check(master, ten, "read65", i2c_master_receive(dev, actual, 65, 1000) == ESP_OK);
    check(master, ten, "read65_bytes", !memcmp(actual, peer.response, 65));
    memset(actual, 0, sizeof(actual));
#if CONFIG_I2C_NATIVE_PROFILE_WIRE_DEFINED
    i2c_device_config_t explicit_dc = dc;
    explicit_dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    explicit_dc.device_address = I2C_DEVICE_ADDRESS_NOT_USED;
    ok = i2c_master_bus_add_device(bus, &explicit_dc, &defined) == ESP_OK;
    check(master, ten, "new_defined_device", ok);
    if (!ok) goto done;
    static uint8_t header[2] = {0, 0xa2}, read_header;
    header[0] = ten ? 0xf2 : 0x84;
    read_header = ten ? 0xf3 : 0x85;
    static i2c_operation_job_t jobs[] = {
        {.command = I2C_MASTER_CMD_START},
        {.command = I2C_MASTER_CMD_WRITE, .write = {.ack_check = true,
            .data = header, .total_bytes = 1}},
        {.command = I2C_MASTER_CMD_WRITE, .write = {.ack_check = true,
            .data = sent, .total_bytes = 17}},
        {.command = I2C_MASTER_CMD_START},
        {.command = I2C_MASTER_CMD_WRITE, .write = {.ack_check = true,
            .data = &read_header, .total_bytes = 1}},
        {.command = I2C_MASTER_CMD_READ, .read = {.ack_value = I2C_ACK_VAL,
            .data = actual, .total_bytes = 16}},
        {.command = I2C_MASTER_CMD_READ, .read = {.ack_value = I2C_NACK_VAL,
            .data = actual + 16, .total_bytes = 1}},
        {.command = I2C_MASTER_CMD_STOP},
    };
    jobs[1].write.total_bytes = ten ? 2 : 1;
    printf("I2C_WIRE_API master=%u ten=%u name=i2c_master_execute_defined_operations commands=8\n", master, ten);
    check(master, ten, "restart17", i2c_master_execute_defined_operations(
        defined, jobs, sizeof(jobs) / sizeof(jobs[0]), 1000) == ESP_OK);
#else
    check(master, ten, "restart17", i2c_master_transmit_receive(dev, sent, 17, actual, 17, 1000) == ESP_OK);
#endif
    check(master, ten, "restart17_bytes", !memcmp(actual, peer.response, 17));
    vTaskDelay(pdMS_TO_TICKS(10));
    portENTER_CRITICAL(&peer.lock);
    bool receive_ok = !peer.overflow && peer.length == 82 && !memcmp(peer.received, sent, 65) &&
                      !memcmp(peer.received + 65, sent, 17);
    unsigned callbacks = peer.callbacks, requests = peer.requests, served = peer.served;
    portEXIT_CRITICAL(&peer.lock);
    check(master, ten, "receive_bytes", receive_ok);
    check(master, ten, "receive_ownership", callbacks == 2);
    check(master, ten, "request_ownership", requests == 2 && served == 2);
    printf("I2C_WIRE_BYTES master=%u ten=%u receive=", master, ten);
    for (unsigned i = 0; i < peer.length; i++) printf("%02x", peer.received[i]);
    printf("\n");
done:
    if (defined) {
        ok = i2c_master_bus_rm_device(defined) == ESP_OK;
        check(master, ten, "remove_defined_device", ok);
        released &= ok;
    }
    if (dev) {
        ok = i2c_master_bus_rm_device(dev) == ESP_OK;
        check(master, ten, "remove_device", ok);
        released &= ok;
    }
    if (bus) {
        ok = i2c_del_master_bus(bus) == ESP_OK;
        check(master, ten, "delete_master", ok);
        released &= ok;
    }
    if (created == pdPASS) {
        portENTER_CRITICAL(&peer.lock);
        peer.quit = true;
        portEXIT_CRITICAL(&peer.lock);
        xTaskNotifyGive(peer.task);
        for (;;) {
            portENTER_CRITICAL(&peer.lock);
            bool exited = peer.exited;
            portEXIT_CRITICAL(&peer.lock);
            if (exited) break;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    ok = i2c_del_slave_device(peer.handle) == ESP_OK;
    check(master, ten, "delete_slave", ok);
    if (ok && created == pdPASS) vTaskDelete(peer.task);
    /* A failed teardown retains callback context/task/buffers; do not reuse them. */
    return released && ok;
}
void app_main(void)
{
    printf("I2C_NATIVE_BOOT profile=%s\n", profile);
    for (unsigned master = 0; master < 2; master++)
        for (unsigned ten = 0; ten < 2; ten++)
            if (!run(master, ten)) goto report;
report:
    printf("I2C_NATIVE_DONE profile=%s failures=%u result=%s\n", profile, failures, failures ? "FAIL" : "PASS");
    fflush(stdout);
}
