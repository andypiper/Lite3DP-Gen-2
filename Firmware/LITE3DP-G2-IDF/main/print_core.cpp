#include "print_core.h"
#include "motor.h"
#include "uv_led.h"
#include "slicer.h"
#include "shared.h"
#include "pins.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <cstring>
#include <cmath>
#include <cstdio>

static const char *TAG = "print";

// ── Shared with ui.cpp ────────────────────────────────────────────────────
extern TFT_eSPI tft;
extern PNG       png;
extern int16_t   g_xpos, g_ypos;

// ── SD card ───────────────────────────────────────────────────────────────

static sdmmc_card_t *s_card = NULL;
static bool          s_sd_mounted = false;

bool sd_card_present(void) {
    return gpio_get_level(PIN_SD_DET) == 0;   // LOW = card inserted
}

bool sd_mount(void) {
    if (s_sd_mounted) return true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 16000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)PIN_SD_CS;
    slot.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sd_mount failed: %s", esp_err_to_name(err));
        return false;
    }
    s_sd_mounted = true;
    ESP_LOGI(TAG, "SD mounted");
    return true;
}

void sd_unmount(void) {
    if (!s_sd_mounted) return;
    esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    s_sd_mounted = false;
}

// ── PNG decode callback ───────────────────────────────────────────────────

void png_draw_cb(PNGDRAW *pDraw) {
    uint16_t lineBuf[480];
    png.getLineAsRGB565(pDraw, lineBuf, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
    tft.pushImage(g_xpos, g_ypos + pDraw->y, pDraw->iWidth, 1, lineBuf);
}

static void display_png_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "missing: %s", path); return; }
    fclose(f);

    int rc = png.open(path,
        [](const char *fn, int32_t *sz) -> void* {
            FILE *f = fopen(fn, "rb");
            if (!f) return nullptr;
            fseek(f, 0, SEEK_END); *sz = ftell(f); rewind(f);
            return f;
        },
        [](void *h){ if (h) fclose((FILE*)h); },
        [](PNGFILE*, uint8_t *b, int32_t l) -> int32_t {
            return (int32_t)fread(b, 1, l, (FILE*)png.getHandle());
        },
        [](PNGFILE*, int32_t pos) -> int32_t {
            return fseek((FILE*)png.getHandle(), pos, SEEK_SET);
        },
        png_draw_cb);

    if (rc == PNG_SUCCESS) {
        png.decode(nullptr, 0);
        png.close();
    }
}

// ── Motor task (Core 1) ───────────────────────────────────────────────────

void motor_task(void *arg) {
    motor_init();

    motor_cmd_t cmd;
    while (true) {
        if (xQueueReceive(g_motor_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd.type) {
                case MOTOR_CMD_HOME:
                    motor_home(cmd.period_us);
                    break;
                case MOTOR_CMD_MOVE:
                    motor_move_steps(cmd.up, cmd.steps, cmd.period_us);
                    break;
            }
            uint8_t done = 1;
            xQueueSend(g_motor_done_queue, &done, 0);
        }
    }
}

// ── Print task helpers ────────────────────────────────────────────────────

static void send_status(print_status_type_t type, int layer, int total, uint32_t ms) {
    print_status_t s = {};
    s.type = type;
    if (type == STATUS_LAYER_DONE) {
        s.progress.layer      = layer;
        s.progress.total      = total;
        s.progress.elapsed_ms = ms;
    }
    xQueueSend(g_print_status_queue, &s, 0);
}

// Send a motor command and wait for it to complete
static void motor_blocking(bool up, long steps, int period_us) {
    motor_cmd_t cmd = { .type = MOTOR_CMD_MOVE, .up = up,
                        .steps = steps, .period_us = period_us };
    xQueueSend(g_motor_cmd_queue, &cmd, portMAX_DELAY);
    uint8_t done;
    xQueueReceive(g_motor_done_queue, &done, portMAX_DELAY);
}

static void motor_home_blocking(int period_us) {
    motor_cmd_t cmd = { .type = MOTOR_CMD_HOME, .period_us = period_us };
    xQueueSend(g_motor_cmd_queue, &cmd, portMAX_DELAY);
    uint8_t done;
    xQueueReceive(g_motor_done_queue, &done, portMAX_DELAY);
}

static void do_calibrate(const print_params_t *p) {
    g_print_state = PRINT_STATE_HOMING;
    if (!motor_endstop()) {
        motor_home_blocking(speed_to_period_us(p->retractSpeed));
    }
    vTaskDelay(pdMS_TO_TICKS(600));

    // Apply levelling offset (stepsAdditional × 80 micro-steps downward)
    long offset_steps = p->stepsAdditional * 80;
    if (offset_steps > 0) {
        motor_blocking(false, offset_steps, speed_to_period_us(p->liftSpeedInit));
    }
    motor_enable(false);    // de-energise after home to reduce heat
}

// Check for a pause/cancel command between layers.
// Returns false if the print was cancelled.
static bool check_pause(float actual_height_mm, const print_params_t *p) {
    print_cmd_t cmd;
    if (xQueuePeek(g_print_cmd_queue, &cmd, 0) != pdTRUE) return true;
    if (cmd.type != CMD_PRINT_PAUSE && cmd.type != CMD_PRINT_CANCEL) return true;

    xQueueReceive(g_print_cmd_queue, &cmd, 0);  // consume

    if (cmd.type == CMD_PRINT_CANCEL) {
        send_status(STATUS_PRINT_COMPLETE, 0, 0, 0);
        return false;
    }

    // Pause: lift to safe height
    float height_to_top = (float)MAX_HEIGHT_MM - actual_height_mm;
    if (height_to_top > 0) {
        motor_blocking(true,
            (long)(STEPS_PER_MM * height_to_top),
            speed_to_period_us(p->liftSpeed));
    }

    g_print_state = PRINT_STATE_PAUSED;
    send_status(STATUS_PAUSED, 0, 0, 0);

    // Wait for RESUME or CANCEL
    while (xQueueReceive(g_print_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        if (cmd.type == CMD_PRINT_RESUME) { g_print_state = PRINT_STATE_PRINTING; break; }
        if (cmd.type == CMD_PRINT_CANCEL) {
            send_status(STATUS_PRINT_COMPLETE, 0, 0, 0);
            // Return to avoid continuing the loop; caller checks return value
            if (height_to_top > 0) {
                motor_blocking(false,
                    (long)(STEPS_PER_MM * height_to_top),
                    speed_to_period_us(p->retractSpeed));
            }
            return false;
        }
    }

    // Return to print position
    if (height_to_top > 0) {
        motor_blocking(false,
            (long)(STEPS_PER_MM * height_to_top),
            speed_to_period_us(p->retractSpeed));
    }
    return true;
}

// ── Layer display ─────────────────────────────────────────────────────────

static void display_layer(const char *folder, int layer_idx,
                           slicer_type_t slicer, int hLayer_um, int print_mode) {
    tft.setRotation(2);
    g_xpos = 0; g_ypos = 27;

    if (print_mode == 1) {
        // Keychain test print — hardcoded assets included from header
        // (AA, AB, AC, AD arrays from keychain.h)
        // Layers are split into 4 sections; pick based on index
        // Placeholder: UI task loads keychain PNGs from FLASH, not SD
        return;
    }
    if (print_mode == 2) {
        // Touch pencil — geometry computed at print time in UI task
        return;
    }

    // Normal SD print
    char path[80];   // Prusa with 24-char folder needs 67 bytes
    slicer_layer_path(path, sizeof(path), folder, layer_idx, slicer);
    display_png_file(path);
}

// ── Main print sequence ───────────────────────────────────────────────────

static void do_print(const print_cmd_t *cmd) {
    const print_params_t &p = g_params;  // snapshot at job start

    int hLayer_um = (int)(p.hLayer * 1000.0f);
    slicer_type_t slicer = slicer_detect(cmd->start.folder);

    // Total printable layers (accounts for layer-height skip)
    int raw = cmd->start.layer_count;
    int total_layers;
    switch (hLayer_um) {
        case 25:  total_layers = raw;          break;
        case 50:  total_layers = raw / 2;      break;
        case 100: total_layers = (raw + 3) / 4; break;
        default:  total_layers = raw;          break;
    }
    if (total_layers <= 0) return;

    g_print_state   = PRINT_STATE_PRINTING;
    g_layer_current = 0;
    g_layer_total   = total_layers;

    int first  = (cmd->start.print_mode != 0) ? 5 : p.firstLayers;
    int trans  = (cmd->start.print_mode != 0) ? 0 : p.traLayers;

    // Derived distances
    float hDown        = p.hUp        - p.hLayer;
    float hDownInitial = p.hUpInitial - p.hLayer;

    // Step period in µs for each speed
    int pLift     = speed_to_period_us(p.liftSpeed);
    int pLiftInit = speed_to_period_us(p.liftSpeedInit);
    int pRetract  = speed_to_period_us(p.retractSpeed);

    const int TOP_DELAY_MS = 200;

    do_calibrate(&p);
    vTaskDelay(pdMS_TO_TICKS(500));
    motor_blocking(true, (long)(STEPS_PER_MM * p.hLayer), pRetract);
    vTaskDelay(pdMS_TO_TICKS(4000));

    uint32_t t_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    float actual_height = p.hLayer;
    int layer_file_idx  = 0;   // file index within the folder (slicer-relative)

    for (int l = 0; l < total_layers; l++) {
        vTaskDelay(pdMS_TO_TICKS(p.restTime));

        // ── Determine exposure time for this layer ─────────────────────────
        uint32_t expo_ms;
        float hup_mm;
        float hdown_mm;
        int   p_lift;

        if (l < first) {
            expo_ms   = (uint32_t)(p.iExpoTime * 1000);
            hup_mm    = p.hUpInitial;
            hdown_mm  = hDownInitial;
            p_lift    = pLiftInit;
        } else if (l < first + trans) {
            int t = l - first + 1;
            float lapse  = (float)(p.iExpoTime - p.expoTime) / (trans + 1);
            float t_expo = p.iExpoTime - lapse * t;
            expo_ms   = (uint32_t)(t_expo * 1000.0f);
            hup_mm    = p.hUpInitial;
            hdown_mm  = hDownInitial;
            p_lift    = pLiftInit;
        } else {
            expo_ms   = (uint32_t)(p.expoTime * 1000.0f);
            hup_mm    = p.hUp;
            hdown_mm  = hDown;
            p_lift    = pLift;
        }

        // ── Display layer image ────────────────────────────────────────────
        display_layer(cmd->start.folder, layer_file_idx, slicer,
                      hLayer_um, cmd->start.print_mode);

        // layer_file_idx step: each firmware layer = (hLayer_um / 25) raw files
        layer_file_idx += hLayer_um / 25;

        // ── Expose ────────────────────────────────────────────────────────
        uv_expose(expo_ms, (uint8_t)p.pwmUV);

        // ── Progress ───────────────────────────────────────────────────────
        tft.setRotation(3);
        g_xpos = 27; g_ypos = 0;
        uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS - t_start;
        g_layer_current = l + 1;
        send_status(STATUS_LAYER_DONE, l + 1, total_layers, elapsed);

        // ── Pause / cancel check ───────────────────────────────────────────
        if (!check_pause(actual_height, &p)) return;

        // ── Lift and retract ───────────────────────────────────────────────
        motor_blocking(true,
            (long)(STEPS_PER_MM * hup_mm), p_lift);
        vTaskDelay(pdMS_TO_TICKS(TOP_DELAY_MS));
        motor_blocking(false,
            (long)(STEPS_PER_MM * hdown_mm), pRetract);

        actual_height += p.hLayer;
    }

    // ── Lift to safe height ────────────────────────────────────────────────
    float remaining = (float)MAX_HEIGHT_MM - actual_height;
    if (remaining > 0) {
        motor_blocking(true, (long)(STEPS_PER_MM * remaining), pLift);
    }

    uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS - t_start;
    g_print_state = PRINT_STATE_IDLE;
    send_status(STATUS_PRINT_COMPLETE, total_layers, total_layers, elapsed);
}

// ── Clean vat ─────────────────────────────────────────────────────────────

static void do_clean_vat(int seconds) {
    tft.fillScreen(TFT_WHITE);
    uv_expose((uint32_t)seconds * 1000, (uint8_t)g_params.pwmUV);
    send_status(STATUS_CLEAN_DONE, 0, 0, 0);
}

// ── Factory test ──────────────────────────────────────────────────────────

static void do_test(void) {
    // UV test
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.fillCircle(240, 140, 30, TFT_WHITE);
    tft.drawCentreString("UV LIGHT ON?", 240, 25, 4);
    uv_set((uint8_t)g_params.pwmUV);
    vTaskDelay(pdMS_TO_TICKS(1500));
    uv_set(0);

    // Motor test
    tft.fillScreen(TFT_BLACK);
    tft.drawCentreString("STEPPER MOTOR TEST", 240, 25, 4);
    if (!motor_endstop()) {
        motor_home_blocking(speed_to_period_us(g_params.retractSpeed));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    motor_blocking(true, (long)(STEPS_PER_MM * 50.0f),
                   speed_to_period_us(g_params.retractSpeed));
    motor_enable(false);

    // SD test
    tft.fillScreen(TFT_BLACK);
    tft.drawCentreString("SD CARD TEST", 240, 25, 4);
    vTaskDelay(pdMS_TO_TICKS(500));

    if (!sd_mount()) {
        tft.drawCentreString("SD MOUNT FAILED", 240, 160, 4);
    } else {
        // Display example.png from SD root
        g_xpos = 0; g_ypos = 27;
        tft.setRotation(2);
        display_png_file("/sdcard/example.png");
        tft.setRotation(3);
        g_xpos = 27; g_ypos = 0;
        tft.setTextColor(TFT_YELLOW);
        tft.drawCentreString("SD IMAGE DISPLAYED?", 240, 25, 4);
    }

    send_status(STATUS_TEST_DONE, 0, 0, 0);
}

// ── Print task (Core 0) ───────────────────────────────────────────────────

void print_task(void *arg) {
    print_cmd_t cmd;
    while (true) {
        if (xQueueReceive(g_print_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        switch (cmd.type) {
            case CMD_CALIBRATE: {
                do_calibrate(&g_params);
                // Lift 50 mm to clear the vat after levelling
                motor_blocking(true,
                    (long)(STEPS_PER_MM * 50.0f),
                    speed_to_period_us(g_params.retractSpeed));
                send_status(STATUS_CALIBRATED, 0, 0, 0);
                break;
            }
            case CMD_MOVE_UP: {
                float mm = cmd.move.mm;
                motor_blocking(true,
                    (long)(STEPS_PER_MM * mm),
                    speed_to_period_us(g_params.retractSpeed));
                send_status(STATUS_MOVE_DONE, 0, 0, 0);
                break;
            }
            case CMD_PRINT_START:
                if (sd_mount()) {
                    do_print(&cmd);
                } else {
                    print_status_t s = {};
                    s.type = STATUS_ERROR;
                    strncpy(s.error.msg, "SD mount failed", sizeof(s.error.msg));
                    xQueueSend(g_print_status_queue, &s, 0);
                }
                break;
            case CMD_CLEAN_VAT:
                do_clean_vat(cmd.clean.seconds);
                break;
            case CMD_TEST_MODE:
                do_test();
                break;
            default:
                break;
        }
    }
}
