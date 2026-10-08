#include <stdio.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint8_t crc8(const uint8_t *bytes)
{
    uint8_t crc = 0;
    for (int i = 0; i < 2; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
    }
    return crc;
}

void app_main(void)
{
    i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0, .sda_io_num = 8, .scl_io_num = 9,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x40, .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t sensor;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device, &sensor));
    const esp_err_t missing = i2c_master_probe(bus, 0x42, 200);
    printf("I2C_NACK_%s\n", missing == ESP_ERR_NOT_FOUND ? "OK" : "FAIL");
    for (;;) {
        uint8_t command = 0xE3, response[3] = {0};
        const esp_err_t status = i2c_master_transmit_receive(sensor, &command, 1, response, 3, 1000);
        if (status == ESP_OK) {
            const unsigned raw = ((response[0] << 8) | response[1]) & ~3u;
            const int centi = (int)(raw * 17572u / 65536u) - 4685;
            printf("I2C_TEMP %d crc=%s\n", centi, crc8(response) == response[2] ? "ok" : "bad");
        } else printf("I2C_ERROR %s\n", esp_err_to_name(status));
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
