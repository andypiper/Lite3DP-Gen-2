#pragma once
#include "shared.h"

// Motor task (Core 1): receives motor_cmd_t from g_motor_cmd_queue,
// executes moves via gptimer ISR, sends done signal to g_motor_done_queue.
void motor_task(void *arg);

// Print/command task (Core 0): receives print_cmd_t from g_print_cmd_queue,
// executes print sequences and utility functions, sends print_status_t to
// g_print_status_queue for the UI task to consume.
void print_task(void *arg);

// SD card mount/unmount helpers
bool     sd_mount(void);
void     sd_unmount(void);
bool     sd_card_present(void);

// PNG decode callback (draw to TFT) — declared here so ui.cpp can use the
// same function for the layer preview.
#include <PNGdec.h>
void png_draw_cb(PNGDRAW *pDraw);
