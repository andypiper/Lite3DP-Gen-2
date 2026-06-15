#include "uv_led.h"
#include "pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>   // ledcAttach / ledcWrite (arduino-esp32 3.x API)

#define UV_FREQ_HZ   5000
#define UV_BITS      8

void uv_init(void) {
    ledcAttach(PIN_UV, UV_FREQ_HZ, UV_BITS);
    ledcWrite(PIN_UV, 0);
}

void uv_set(uint8_t duty) {
    ledcWrite(PIN_UV, duty);
}

void uv_expose(uint32_t ms, uint8_t duty) {
    ledcWrite(PIN_UV, duty);
    // vTaskDelay yields to the RTOS scheduler — fixes the TWDT issue
    // that plagued the original delay()-based exposure.
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledcWrite(PIN_UV, 0);
}
