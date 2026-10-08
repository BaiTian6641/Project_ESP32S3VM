/* SPDX-License-Identifier: Apache-2.0
 * Ordinary IDF master-driver fixture. The runner supplies the electrical graph;
 * these profile selections specify expectations, never install virtual devices.
 * Connected: separate SHT21 (0x40) and 256-byte/16-byte-page EEPROM (0x50)
 * on each controller, with external SDA/SCL pull-ups to 3.3V and common ground.
 * EEPROM writes deliberately exercise page wrap as well as FIFO-sized traffic.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TIMEOUT_MS 250
#define EEPROM_BASE 0x80
#define EEPROM_BYTES 48
static unsigned failures;

static void check(unsigned port, const char *name, bool ok)
{
    printf("I2C_NATIVE_CHECK port=%u name=%s result=%s\n", port, name, ok ? "PASS" : "FAIL");
    failures += !ok;
    fflush(stdout);
}

static bool status(unsigned port, const char *name, esp_err_t actual, esp_err_t expected)
{
    printf("I2C_NATIVE_STATUS port=%u name=%s actual=%s expected=%s\n",
           port, name, esp_err_to_name(actual), esp_err_to_name(expected));
    check(port, name, actual == expected);
    return actual == expected;
}

#if CONFIG_I2C_NATIVE_PROFILE_CONNECTED
static void bytes(unsigned port, const char *name, const uint8_t *data, size_t length)
{
    printf("I2C_NATIVE_BYTES port=%u name=%s data=", port, name);
    for (size_t i = 0; i < length; ++i) printf("%02x", data[i]);
    printf("\n");
    fflush(stdout);
}

static uint8_t crc8(const uint8_t data[2])
{
    uint8_t crc = 0;
    for (unsigned i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static bool sample(unsigned port, i2c_master_dev_handle_t sensor,
                   uint8_t command, unsigned wait_ms, uint16_t *raw)
{
    uint8_t response[3] = {0};
    int64_t start = esp_timer_get_time();
    if (!status(port, "conversion_command", i2c_master_transmit(sensor, &command, 1, TIMEOUT_MS), ESP_OK)) return false;
    /* No-hold mode must not fabricate a sample before conversion completes. */
    status(port, "conversion_busy_nack", i2c_master_receive(sensor, response, sizeof(response), TIMEOUT_MS), ESP_ERR_INVALID_RESPONSE);
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    if (!status(port, "conversion_read", i2c_master_receive(sensor, response, sizeof(response), TIMEOUT_MS), ESP_OK)) return false;
    int64_t elapsed = esp_timer_get_time() - start;
    bytes(port, command == 0xf3 ? "temperature" : "humidity", response, sizeof(response));
    printf("I2C_NATIVE_CONVERSION port=%u command=%02x elapsed_us=%" PRId64 "\n", port, command, elapsed);
    check(port, "conversion_delay", elapsed >= (int64_t)wait_ms * 1000);
    check(port, "sample_crc8", crc8(response) == response[2]);
    check(port, "sample_status", (response[1] & 3) == (command == 0xf5 ? 2 : 0));
    *raw = ((uint16_t)response[0] << 8 | response[1]) & 0xfffc;
    return true;
}

static void sensor_tests(unsigned port, i2c_master_dev_handle_t sensor)
{
    uint8_t unsupported = 0x00;
    status(port, "unsupported_data_nack", i2c_master_transmit(sensor, &unsupported, 1, TIMEOUT_MS), ESP_ERR_INVALID_RESPONSE);
    uint16_t first = 0, second = 0, humidity = 0;
    bool first_ok = sample(port, sensor, 0xf3, 100, &first);
    bool second_ok = sample(port, sensor, 0xf3, 100, &second);
    check(port, "consecutive_conversion_change", first_ok && second_ok && first != second);
    sample(port, sensor, 0xf5, 40, &humidity);
}

static void eeprom_tests(unsigned port, i2c_master_dev_handle_t eeprom)
{
    uint8_t expected[EEPROM_BYTES], readback[EEPROM_BYTES] = {0}, separate[EEPROM_BYTES] = {0};
    for (unsigned i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)(i * 7 + port * 19 + 3);
    for (unsigned offset = 0; offset < sizeof(expected); offset += 16) {
        uint8_t page[17];
        page[0] = EEPROM_BASE + offset;
        memcpy(page + 1, expected + offset, 16);
        if (!status(port, "page_write", i2c_master_transmit(eeprom, page, sizeof(page), TIMEOUT_MS), ESP_OK)) return;
        vTaskDelay(pdMS_TO_TICKS(6));
    }
    uint8_t pointer = EEPROM_BASE;
    if (!status(port, "initial_repeated_start_read", i2c_master_transmit_receive(eeprom, &pointer, 1, readback, sizeof(readback), TIMEOUT_MS), ESP_OK)) return;
    bytes(port, "initial_eeprom48", readback, sizeof(readback));
    check(port, "initial_eeprom48_matches", memcmp(expected, readback, sizeof(expected)) == 0);

    /* A 65-byte transfer crosses the hardware FIFO boundary. The EEPROM
     * wraps its 16-byte page, so only the final 16 payload bytes survive. */
    uint8_t burst[65];
    burst[0] = EEPROM_BASE;
    for (unsigned i = 0; i < 64; ++i) burst[i + 1] = (uint8_t)(0xa0 + i + port);
    if (!status(port, "burst65_write", i2c_master_transmit(eeprom, burst, sizeof(burst), TIMEOUT_MS), ESP_OK)) return;
    vTaskDelay(pdMS_TO_TICKS(6));
    memcpy(expected, burst + 49, 16);
    if (!status(port, "repeated_start_read48", i2c_master_transmit_receive(eeprom, &pointer, 1, readback, sizeof(readback), TIMEOUT_MS), ESP_OK)) return;
    bytes(port, "changed_eeprom48", readback, sizeof(readback));
    check(port, "eeprom_page_wrap_and_change", memcmp(expected, readback, sizeof(expected)) == 0);
    if (!status(port, "stop_pointer_write", i2c_master_transmit(eeprom, &pointer, 1, TIMEOUT_MS), ESP_OK)) return;
    if (!status(port, "stop_separated_read48", i2c_master_receive(eeprom, separate, sizeof(separate), TIMEOUT_MS), ESP_OK)) return;
    bytes(port, "stop_eeprom48", separate, sizeof(separate));
    check(port, "restart_vs_stop", memcmp(readback, separate, sizeof(readback)) == 0 && memcmp(expected, separate, sizeof(expected)) == 0);

    /* Full-page write followed by RESTART wraps the pointer back to the page
     * start. Data read before STOP must still be the old committed page. */
    uint8_t staged[17], before_stop[16] = {0};
    staged[0] = EEPROM_BASE;
    for (unsigned i = 0; i < 16; ++i) staged[i + 1] = (uint8_t)(0x50 + i + port);
    if (!status(port, "staged_write_restart_read", i2c_master_transmit_receive(eeprom, staged, sizeof(staged), before_stop, sizeof(before_stop), TIMEOUT_MS), ESP_OK)) return;
    bytes(port, "before_stop_page16", before_stop, sizeof(before_stop));
    check(port, "restart_does_not_commit", memcmp(before_stop, expected, sizeof(before_stop)) == 0);
    /* transmit_receive ends in STOP: that STOP commits the staged page. */
    vTaskDelay(pdMS_TO_TICKS(6));
    memcpy(expected, staged + 1, 16);
    if (!status(port, "committed_after_stop_read", i2c_master_transmit_receive(eeprom, &pointer, 1, readback, sizeof(readback), TIMEOUT_MS), ESP_OK)) return;
    bytes(port, "after_stop_eeprom48", readback, sizeof(readback));
    check(port, "stop_commits_staged_page", memcmp(expected, readback, sizeof(expected)) == 0 && memcmp(before_stop, readback, sizeof(before_stop)) != 0);
}

#endif

static const char *profile(void)
{
#if CONFIG_I2C_NATIVE_PROFILE_WRONG
    return "wrong";
#elif CONFIG_I2C_NATIVE_PROFILE_DISCONNECTED
    return "disconnected";
#elif CONFIG_I2C_NATIVE_PROFILE_POWER
    return "power";
#elif CONFIG_I2C_NATIVE_PROFILE_NO_PULL
    return "no_pull";
#elif CONFIG_I2C_NATIVE_PROFILE_STUCK
    return "stuck";
#else
    return "connected";
#endif
}

void app_main(void)
{
    const int sda[2] = {CONFIG_I2C_NATIVE_SDA0, CONFIG_I2C_NATIVE_SDA1};
    const int scl[2] = {CONFIG_I2C_NATIVE_SCL0, CONFIG_I2C_NATIVE_SCL1};
#if CONFIG_I2C_NATIVE_PROFILE_NO_PULL
    const char *pullup = "none";
#else
    const char *pullup = "external";
#endif
    printf("I2C_NATIVE_BOOT profile=%s\n", profile());
    for (unsigned port = 0; port < 2; ++port) {
        printf("I2C_NATIVE_PHASE port=%u profile=%s sda=%d scl=%d speed_hz=%d pullup=%s\n",
               port, profile(), sda[port], scl[port], CONFIG_I2C_NATIVE_SPEED_HZ,
               pullup);
        fflush(stdout);
        i2c_master_bus_config_t config = {
            .i2c_port = (i2c_port_num_t)port,
            .sda_io_num = (gpio_num_t)sda[port], .scl_io_num = (gpio_num_t)scl[port],
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = false,
        };
        i2c_master_bus_handle_t bus = NULL;
        if (!status(port, "new_bus", i2c_new_master_bus(&config, &bus), ESP_OK)) continue;
#if CONFIG_I2C_NATIVE_PROFILE_CONNECTED
        status(port, "probe_sht21", i2c_master_probe(bus, 0x40, TIMEOUT_MS), ESP_OK);
        status(port, "probe_eeprom", i2c_master_probe(bus, 0x50, TIMEOUT_MS), ESP_OK);
        status(port, "absent_address_nack", i2c_master_probe(bus, 0x51, TIMEOUT_MS), ESP_ERR_NOT_FOUND);
        i2c_device_config_t device = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x40, .scl_speed_hz = CONFIG_I2C_NATIVE_SPEED_HZ,
        };
        i2c_master_dev_handle_t sensor = NULL, eeprom = NULL;
        if (status(port, "add_sensor", i2c_master_bus_add_device(bus, &device, &sensor), ESP_OK)) {
            sensor_tests(port, sensor);
            status(port, "remove_sensor", i2c_master_bus_rm_device(sensor), ESP_OK);
        }
        device.device_address = 0x50;
        if (status(port, "add_eeprom", i2c_master_bus_add_device(bus, &device, &eeprom), ESP_OK)) {
            eeprom_tests(port, eeprom);
            status(port, "remove_eeprom", i2c_master_bus_rm_device(eeprom), ESP_OK);
        }
#else
#if CONFIG_I2C_NATIVE_PROFILE_NO_PULL || CONFIG_I2C_NATIVE_PROFILE_STUCK
        const esp_err_t expected = ESP_ERR_TIMEOUT;
#else
        const esp_err_t expected = ESP_ERR_NOT_FOUND;
#endif
        status(port, "graph_sensor_unreachable", i2c_master_probe(bus, 0x40, TIMEOUT_MS), expected);
        status(port, "graph_eeprom_unreachable", i2c_master_probe(bus, 0x50, TIMEOUT_MS), expected);
#endif
        status(port, "delete_bus", i2c_del_master_bus(bus), ESP_OK);
    }
    printf("I2C_NATIVE_DONE profile=%s failures=%u result=%s\n", profile(), failures, failures ? "FAIL" : "PASS");
    fflush(stdout);
}
