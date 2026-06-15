#pragma once
#include <stdint.h>

void     uv_init(void);
void     uv_set(uint8_t duty);
// Turns UV on at duty, blocks for ms using vTaskDelay (TWDT-safe), then off.
void     uv_expose(uint32_t ms, uint8_t duty);
