#pragma once
// Minimal host-build stub for FreeRTOS types used in shared.h
typedef unsigned int  TickType_t;
typedef void*         QueueHandle_t;
typedef long          BaseType_t;
typedef unsigned long UBaseType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
