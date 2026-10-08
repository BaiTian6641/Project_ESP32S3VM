// SPDX-License-Identifier: MIT
// Ordinary Arduino setup()/loop(); UART markers are test observations only.
#include <Arduino.h>
#include "esp_app_desc.h"

void setup()
{
    Serial.begin(115200);
    pinMode(2, OUTPUT);
    const unsigned cores = ESP.getChipCores();
    const unsigned long flash = ESP.getFlashChipSize();
    Serial.printf("ESP32S3VM_ARDUINO_BOOT_OK arduino=%s idf=%s variant=%s loop_core=%d cores=%u flash=%lu\n",
                  ESP_ARDUINO_VERSION_STR, ESP.getSdkVersion(), ARDUINO_VARIANT,
                  xPortGetCoreID(), cores, flash);
    const esp_app_desc_t *description = esp_app_get_description();
    Serial.print("ESP32S3VM_ARDUINO_ELF_SHA256 ");
    for (unsigned i = 0; i < sizeof(description->app_elf_sha256); ++i) {
        Serial.printf("%02x", description->app_elf_sha256[i]);
    }
    Serial.println();
    // Reuse the existing real-boot/QMP control test without changing its code.
    Serial.printf("ESP32S3VM_BOOT_OK cores=%u flash=%lu\n", cores, flash);
    Serial.flush();
}

void loop()
{
    static unsigned tick;
    digitalWrite(2, tick & 1U);
    Serial.printf("ESP32S3VM_TICK %u time_us=%lu millis=%lu\n", tick,
                  static_cast<unsigned long>(micros()), static_cast<unsigned long>(millis()));
    Serial.flush();
    ++tick;
    delay(500);
}
