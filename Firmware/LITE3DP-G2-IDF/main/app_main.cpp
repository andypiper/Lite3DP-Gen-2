#include "shared.h"
#include "prefs.h"
#include "uv_led.h"
#include "print_core.h"
#include "ui.h"
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

// Queue and global state definitions (declared extern in shared.h)
QueueHandle_t g_motor_cmd_queue;
QueueHandle_t g_motor_done_queue;
QueueHandle_t g_print_cmd_queue;
QueueHandle_t g_print_status_queue;
print_params_t g_params;

extern "C" void app_main(void) {
    // Initialize NVS (required before NVS-backed prefs)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Arduino compatibility layer (initializes Serial, SPI, Wire, etc.)
    initArduino();

    // Load persisted print parameters (slot 0 = Settings)
    prefs_init();
    prefs_load(0, &g_params);

    // UV LED PWM channel setup
    uv_init();

    // Inter-task queues
    g_motor_cmd_queue    = xQueueCreate(4,  sizeof(motor_cmd_t));
    g_motor_done_queue   = xQueueCreate(4,  sizeof(uint8_t));
    g_print_cmd_queue    = xQueueCreate(4,  sizeof(print_cmd_t));
    g_print_status_queue = xQueueCreate(16, sizeof(print_status_t));

    // Core 1: motor step generation (gptimer ISR + blocking semaphore wait)
    xTaskCreatePinnedToCore(motor_task, "motor", 4096,  NULL, 5, NULL, 1);

    // Core 0: print sequencing, SD, PNG decode, UV exposure
    xTaskCreatePinnedToCore(print_task, "print", 8192,  NULL, 4, NULL, 0);

    // Core 0: TFT + touch UI (lower priority so print_task preempts if needed)
    xTaskCreatePinnedToCore(ui_task,    "ui",    16384, NULL, 3, NULL, 0);

    vTaskDelete(NULL);
}
