#include <stdio.h>
#include <stdbool.h>
#include "esp_app_desc.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#if REFERENCE_BLE && !CONFIG_BT_BLUEDROID_ENABLED
#error "This discovery fixture currently qualifies the controller and Bluedroid path only"
#endif
#define CAPTURE_GRACE_MS 15000
static unsigned sequence;

static void event(const char *stage, const char *phase, bool attempted, esp_err_t err)
{
    const char *status = !attempted ? "skipped" : phase[0] == 's' ? "enter" : err == ESP_OK ? "ok" : "error";
    printf("ESP32S3VM_REF {\"schema\":1,\"type\":\"event\",\"seq\":%u,\"stage\":\"%s\","
           "\"phase\":\"%s\",\"attempted\":%s,\"err\":%d,\"status\":\"%s\",\"name\":\"%s\"}\n",
           sequence++, stage, phase, attempted ? "true" : "false", err, status,
           attempted ? esp_err_to_name(err) : "prerequisite failure");
    fflush(stdout);
}

#define CALL(target, name, allowed, expression) do { \
    bool attempted = (allowed); \
    event(name, "start", attempted, ESP_OK); \
    target = attempted ? (expression) : ESP_FAIL; \
    event(name, "result", attempted, target); \
} while (0)

void app_main(void)
{
    /* Same normal application delay on hardware and QEMU; no driver bypass. */
    vTaskDelay(pdMS_TO_TICKS(CAPTURE_GRACE_MS));
    const unsigned count = 2 + (REFERENCE_WIFI ? 14 : 0) + (REFERENCE_BLE ? 16 : 0);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    printf("ESP32S3VM_REF {\"schema\":1,\"type\":\"begin\",\"seq\":%u,\"fixture\":\"radio_init\","
           "\"seed\":0,\"expected_records\":%u,\"capture_grace_ms\":%u,\"idf\":\"%s\",\"chip_revision\":%u,\"cores\":%u,"
           "\"mode\":\"%s\",\"source_sha256\":\"%s\",\"elf_digest\":\"",
           sequence++, count, CAPTURE_GRACE_MS, IDF_VER, chip.revision, chip.cores,
           REFERENCE_RADIO_MODE, REFERENCE_SOURCE_SHA256);
    for (unsigned i = 0; i < 32; ++i) printf("%02x", app->app_elf_sha256[i]);
    printf("\"}\n");
    fflush(stdout);
    esp_err_t nvs;
    /* Do not erase NVS: report errors rather than mutate an existing board silently. */
    CALL(nvs, "nvs_flash_init", true, nvs_flash_init());
#if REFERENCE_WIFI
    esp_err_t netif, events, init, mode, start, stop, deinit;
    CALL(netif, "esp_netif_init", nvs == ESP_OK, esp_netif_init());
    CALL(events, "esp_event_loop_create_default", netif == ESP_OK, esp_event_loop_create_default());
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    CALL(init, "esp_wifi_init", events == ESP_OK, esp_wifi_init(&wifi));
    CALL(mode, "esp_wifi_set_mode", init == ESP_OK, esp_wifi_set_mode(WIFI_MODE_STA));
    CALL(start, "esp_wifi_start", mode == ESP_OK, esp_wifi_start());
    if (start == ESP_OK) vTaskDelay(pdMS_TO_TICKS(200));
    CALL(stop, "esp_wifi_stop", start == ESP_OK, esp_wifi_stop());
    CALL(deinit, "esp_wifi_deinit", init == ESP_OK, esp_wifi_deinit());
    (void)stop;
    (void)deinit;
#endif
#if REFERENCE_BLE
    esp_err_t init_bt, enable_bt, init_host, enable_host, disable_host, deinit_host, disable_bt, deinit_bt;
    esp_bt_controller_config_t bt = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    CALL(init_bt, "esp_bt_controller_init", nvs == ESP_OK, esp_bt_controller_init(&bt));
    CALL(enable_bt, "esp_bt_controller_enable", init_bt == ESP_OK, esp_bt_controller_enable(ESP_BT_MODE_BLE));
    CALL(init_host, "esp_bluedroid_init", enable_bt == ESP_OK, esp_bluedroid_init());
    CALL(enable_host, "esp_bluedroid_enable", init_host == ESP_OK, esp_bluedroid_enable());
    if (enable_host == ESP_OK) vTaskDelay(pdMS_TO_TICKS(200));
    CALL(disable_host, "esp_bluedroid_disable", enable_host == ESP_OK, esp_bluedroid_disable());
    CALL(deinit_host, "esp_bluedroid_deinit", init_host == ESP_OK, esp_bluedroid_deinit());
    CALL(disable_bt, "esp_bt_controller_disable", enable_bt == ESP_OK, esp_bt_controller_disable());
    CALL(deinit_bt, "esp_bt_controller_deinit", init_bt == ESP_OK, esp_bt_controller_deinit());
    (void)disable_host;
    (void)deinit_host;
    (void)disable_bt;
    (void)deinit_bt;
#endif
    printf("ESP32S3VM_REF {\"schema\":1,\"type\":\"end\",\"seq\":%u,\"records\":%u,\"complete\":true}\n",
           sequence, sequence - 1);
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
