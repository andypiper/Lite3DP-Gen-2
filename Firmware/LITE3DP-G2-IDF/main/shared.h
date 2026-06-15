#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ── Hardware constants ─────────────────────────────────────────────────────
#define STEPS_PER_MM     5245    // 200 steps/rev × 64 microsteps / 2.44 mm pitch
#define LGUIDE_MM        115     // linear guide total length (mm)
#define MAX_HEIGHT_MM    80      // usable print height = LGUIDE_MM − 35

// ── Print parameters (stored in NVS, one set per memory slot) ─────────────
typedef struct {
    float hLayer;           // layer height (mm): 0.025, 0.050, 0.100
    float hUp;              // lift height (mm)
    float hUpInitial;       // bottom-layer lift height (mm)
    int   firstLayers;      // number of bottom layers
    int   traLayers;        // number of transition layers
    float expoTime;         // normal exposure (s)
    int   iExpoTime;        // bottom exposure (s)
    float liftSpeed;        // lift speed (mm/s)
    float liftSpeedInit;    // bottom lift speed (mm/s)
    float retractSpeed;     // retract speed (mm/s)
    int   restTime;         // post-retract rest (ms)
    int   pwmUV;            // UV power 0–255
    long  stepsAdditional;  // levelling offset (steps × 80)
} print_params_t;

// ── Slicer format ──────────────────────────────────────────────────────────
typedef enum {
    SLICER_UNKNOWN = 0,
    SLICER_PRUSA,       // foldersel00000.png
    SLICER_LYCHEE4,     // lychee0000.png
    SLICER_LYCHEE3,     // lychee000.png
    SLICER_VOXELDANCE,  // 0.png, 1.png, ...
    SLICER_CHITUBOX,    // 1.png, 2.png, ...
} slicer_type_t;

// ── Commands: main task → motor task ──────────────────────────────────────
typedef enum {
    MOTOR_CMD_MOVE,
    MOTOR_CMD_HOME,
} motor_cmd_type_t;

typedef struct {
    motor_cmd_type_t type;
    bool   up;           // true = ascend, false = descend
    long   steps;
    int    period_us;    // full step period in µs
} motor_cmd_t;

// ── Commands: ui → print_core ─────────────────────────────────────────────
typedef enum {
    CMD_PRINT_START,
    CMD_PRINT_PAUSE,
    CMD_PRINT_RESUME,
    CMD_PRINT_CANCEL,
    CMD_CALIBRATE,
    CMD_MOVE_UP,
    CMD_CLEAN_VAT,
    CMD_TEST_MODE,
} print_cmd_type_t;

typedef struct {
    print_cmd_type_t type;
    union {
        struct { char folder[32]; int layer_count; int print_mode; } start;
        struct { float mm; }   move;
        struct { int seconds; } clean;
    };
} print_cmd_t;

// ── Status: print_core → ui ───────────────────────────────────────────────
typedef enum {
    STATUS_LAYER_DONE,
    STATUS_PRINT_COMPLETE,
    STATUS_PAUSED,
    STATUS_ERROR,
    STATUS_CALIBRATED,
    STATUS_MOVE_DONE,
    STATUS_CLEAN_DONE,
    STATUS_TEST_DONE,
} print_status_type_t;

typedef struct {
    print_status_type_t type;
    union {
        struct { int layer; int total; uint32_t elapsed_ms; } progress;
        struct { char msg[32]; } error;
    };
} print_status_t;

// ── Inter-task queues (defined in app_main.cpp) ───────────────────────────
extern QueueHandle_t g_motor_cmd_queue;
extern QueueHandle_t g_motor_done_queue;
extern QueueHandle_t g_print_cmd_queue;
extern QueueHandle_t g_print_status_queue;

// ── Shared parameters (written by UI, read by print_core at job start) ────
extern print_params_t g_params;

// ── Helpers ───────────────────────────────────────────────────────────────
static inline int speed_to_period_us(float mm_per_s) {
    if (mm_per_s <= 0.0f) mm_per_s = 0.1f;
    return (int)(1000000.0f / (STEPS_PER_MM * mm_per_s));
}
