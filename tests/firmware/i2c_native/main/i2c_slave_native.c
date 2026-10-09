/* SPDX-License-Identifier: Apache-2.0
 * Ordinary slave APIs only. Registered graph peers generate every clock/data
 * edge; GPIO12 is a physically wired host gate, not a register shortcut.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_slave.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HOST_GATE GPIO_NUM_12
#define ADDRESS 0x2a
#define RECEIVE_CAPACITY 256
#define TRANSFER_BYTES 65

#if CONFIG_I2C_NATIVE_PROFILE_SLAVE_DISCONNECTED
static const char *profile = "slave_disconnected";
static const bool disconnected = true;
#else
static const char *profile = "slave";
static const bool disconnected = false;
#endif

static unsigned failures;

typedef struct {
    i2c_slave_dev_handle_t handle;
    TaskHandle_t task;
    portMUX_TYPE lock;
    uint8_t received[RECEIVE_CAPACITY];
    uint32_t length, callbacks, requests;
    bool overflow;
} Slave;

static Slave slaves[2] = {
    {.lock = portMUX_INITIALIZER_UNLOCKED},
    {.lock = portMUX_INITIALIZER_UNLOCKED},
};

static void check(unsigned port, const char *name, bool ok)
{
    printf("I2C_NATIVE_CHECK port=%u name=%s result=%s\n", port, name,
           ok ? "PASS" : "FAIL");
    failures += !ok;
}

static bool status(unsigned port, const char *name, esp_err_t actual)
{
    printf("I2C_NATIVE_STATUS port=%u name=%s actual=%s expected=ESP_OK\n",
           port, name, esp_err_to_name(actual));
    check(port, name, actual == ESP_OK);
    return actual == ESP_OK;
}

static bool received(i2c_slave_dev_handle_t handle,
                     const i2c_slave_rx_done_event_data_t *event, void *opaque)
{
    (void)handle;
    Slave *s = opaque;
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s->lock);
    if (event->length > sizeof(s->received) - s->length) {
        s->overflow = true;
    } else {
        memcpy(s->received + s->length, event->buffer, event->length);
        s->length += event->length;
    }
    ++s->callbacks;
    portEXIT_CRITICAL_ISR(&s->lock);
    vTaskNotifyGiveFromISR(s->task, &woken);
    return woken == pdTRUE;
}

static bool requested(i2c_slave_dev_handle_t handle,
                      const i2c_slave_request_event_data_t *event, void *opaque)
{
    (void)handle;
    (void)event;
    Slave *s = opaque;
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s->lock);
    ++s->requests;
    portEXIT_CRITICAL_ISR(&s->lock);
    vTaskNotifyGiveFromISR(s->task, &woken);
    return woken == pdTRUE;
}

static bool wait_gate(bool high)
{
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (!!gpio_get_level(HOST_GATE) != high && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    bool ready = !!gpio_get_level(HOST_GATE) == high;
    check(0, high ? "host_gate_high" : "host_gate_low", ready);
    return ready;
}

static void payload(unsigned phase, unsigned port, uint8_t *data, unsigned length)
{
    for (unsigned i = 0; i < length; ++i) {
        data[i] = phase == 0 ? (i * 7 + port * 29 + 3) & 255 :
                  phase == 1 ? (i * 11 + port * 17 + 0x50) & 255 :
                               (i * 13 + port * 19 + 0xa0) & 255;
    }
}

static void phase(unsigned number)
{
    const unsigned length = number == 2 ? 17 : TRANSFER_BYTES;
    uint8_t expected[2][TRANSFER_BYTES];
    uint32_t served[2] = {0, 0};
    esp_err_t responses[2] = {ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_STATE};
    uint32_t response_bytes[2] = {0, 0};
    if (!wait_gate(false)) return;
    for (unsigned port = 0; port < 2; ++port) {
        Slave *s = &slaves[port];
        payload(number, port, expected[port], length);
        portENTER_CRITICAL(&s->lock);
        s->length = s->callbacks = s->requests = 0;
        s->overflow = false;
        portEXIT_CRITICAL(&s->lock);
        if (number == 2) {
            uint8_t discarded[49];
            memset(discarded, 0xde, sizeof(discarded));
            uint32_t written = 0;
            status(port, "queue_discarded49", i2c_slave_write(s->handle,
                   discarded, sizeof(discarded), &written, 500));
            check(port, "queued_discarded49", written == sizeof(discarded));
            status(port, "reset_tx_fifo", i2c_slave_reset_tx_fifo(s->handle));
        }
    }
    printf("I2C_SLAVE_READY phase=%u length=%u gate_gpio=12\n", number, length);
    fflush(stdout);
    if (!wait_gate(true)) return;

    int64_t deadline = esp_timer_get_time() +
        (disconnected ? 150000 : 1500000);
    while (esp_timer_get_time() < deadline) {
        bool complete = true;
        for (unsigned port = 0; port < 2; ++port) {
            Slave *s = &slaves[port];
            portENTER_CRITICAL(&s->lock);
            uint32_t requests = s->requests;
            uint32_t count = s->length;
            bool overflow = s->overflow;
            portEXIT_CRITICAL(&s->lock);
            if (number && requests > served[port]) {
                uint32_t written = 0;
                responses[port] = i2c_slave_write(s->handle, expected[port],
                                                  length, &written, 500);
                response_bytes[port] += written;
                served[port] = requests;
            }
            complete &= count == length && !overflow;
        }
        if (!disconnected && complete) break;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }

    for (unsigned port = 0; port < 2; ++port) {
        Slave *s = &slaves[port];
        uint8_t actual[RECEIVE_CAPACITY];
        portENTER_CRITICAL(&s->lock);
        uint32_t count = s->length, callbacks = s->callbacks, requests = s->requests;
        bool overflow = s->overflow;
        memcpy(actual, s->received, count);
        portEXIT_CRITICAL(&s->lock);
        printf("I2C_SLAVE_RESULT port=%u phase=%u callbacks=%" PRIu32
               " requests=%" PRIu32 " length=%" PRIu32 "\n",
               port, number, callbacks, requests, count);
        char name[48];
        if (disconnected) {
            check(port, "disconnected_no_receive", count == 0 && callbacks == 0);
            check(port, "disconnected_no_request", requests == 0);
        } else {
            if (number) {
                if (served[port]) {
                    status(port, "respond_write", responses[port]);
                } else {
                    check(port, "respond_write", false);
                }
                check(port, "respond_written_exact", response_bytes[port] == length);
            }
            snprintf(name, sizeof(name), "slave_phase%u_bytes", number);
            check(port, name, !overflow && count == length &&
                  !memcmp(actual, expected[port], length));
            snprintf(name, sizeof(name), "slave_phase%u_callbacks", number);
            check(port, name, callbacks == 1);
            snprintf(name, sizeof(name), "slave_phase%u_requests", number);
            check(port, name, requests == (number ? 1 : 0));
            printf("I2C_SLAVE_BYTES port=%u phase=%u data=", port, number);
            for (unsigned i = 0; i < count; ++i) printf("%02x", actual[i]);
            printf("\n");
        }
    }
    printf("I2C_SLAVE_FINISHED phase=%u\n", number);
    fflush(stdout);
}

void app_main(void)
{
    const int sda[2] = {CONFIG_I2C_NATIVE_SDA0, CONFIG_I2C_NATIVE_SDA1};
    const int scl[2] = {CONFIG_I2C_NATIVE_SCL0, CONFIG_I2C_NATIVE_SCL1};
    printf("I2C_NATIVE_BOOT profile=%s\n", profile);
    gpio_config_t gate = {.pin_bit_mask = 1ULL << HOST_GATE,
                          .mode = GPIO_MODE_INPUT};
    if (!status(0, "host_gate_input", gpio_config(&gate))) goto done;
    for (unsigned port = 0; port < 2; ++port) {
        printf("I2C_NATIVE_PHASE port=%u profile=%s sda=%d scl=%d speed_hz=%d pullup=external\n",
               port, profile, sda[port], scl[port], CONFIG_I2C_NATIVE_SPEED_HZ);
        slaves[port].task = xTaskGetCurrentTaskHandle();
        i2c_slave_config_t config = {
            .i2c_port = (i2c_port_num_t)port,
            .sda_io_num = (gpio_num_t)sda[port], .scl_io_num = (gpio_num_t)scl[port],
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .send_buf_depth = RECEIVE_CAPACITY, .receive_buf_depth = RECEIVE_CAPACITY,
            .slave_addr = ADDRESS, .addr_bit_len = I2C_ADDR_BIT_LEN_7,
            .flags.enable_internal_pullup = false,
        };
        if (!status(port, "new_slave", i2c_new_slave_device(&config, &slaves[port].handle))) goto done;
        i2c_slave_event_callbacks_t callbacks = {.on_request = requested,
                                                .on_receive = received};
        if (!status(port, "slave_callbacks", i2c_slave_register_event_callbacks(
                    slaves[port].handle, &callbacks, &slaves[port]))) goto done;
    }
    for (unsigned number = 0; number <
         (disconnected ? 1 : 3); ++number) {
        phase(number);
    }
done:
    for (unsigned port = 0; port < 2; ++port) {
        if (slaves[port].handle) {
            status(port, "delete_slave", i2c_del_slave_device(slaves[port].handle));
        }
    }
    printf("I2C_NATIVE_DONE profile=%s failures=%u result=%s\n",
           profile, failures, failures ? "FAIL" : "PASS");
    fflush(stdout);
}
