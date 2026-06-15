/*
 * UI task — menu state machine and display rendering.
 * Runs on Core 0. Sends commands to print_task via g_print_cmd_queue and
 * receives status from g_print_status_queue.
 *
 * Screen numbering is preserved from the original firmware for reference.
 */
#include "ui.h"
#include "shared.h"
#include "pins.h"
#include "prefs.h"
#include "params.h"
#include "slicer.h"
#include "print_core.h"
#include "wifi_mgr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <PNGdec.h>
#include <Arduino.h>

// Asset arrays (from original firmware header files)
#include "menus.h"
#include "templatesDG.h"
#include "template12234.h"
#include "keychain.h"

static const char *TAG = "ui";

// ── Globals shared with print_core.cpp ────────────────────────────────────
TFT_eSPI tft;
PNG      png;
int16_t  g_xpos = 27, g_ypos = 0;

// ── Touch controller ───────────────────────────────────────────────────────
static XPT2046_Touchscreen ts(PIN_T_CS, PIN_T_IRQ);

// ── Touch calibration (raw ADC → pixel) ───────────────────────────────────
#define TS_MINX 240
#define TS_MINY 3800
#define TS_MAXX 3700
#define TS_MAXY 360

// ── Menu state ─────────────────────────────────────────────────────────────
typedef enum {
    SCR_MAIN        = 0,
    SCR_SETTINGS    = 1,
    SCR_UTILITIES   = 2,
    SCR_FILE_SEL    = 11,
    SCR_PARAM_SEL   = 14,
    // Parameter slot sub-screens
    SCR_PARAM_CUR   = 14,
    SCR_PARAM_CHG   = 141,
    SCR_PARAM_A     = 142,
    SCR_PARAM_B     = 143,
    SCR_PARAM_C     = 144,
    SCR_PARAM_D     = 145,
    SCR_PARAM_E     = 146,
    SCR_PARAM_F     = 147,
    // Pre-print confirmation
    SCR_CONF_LOAD   = 15,
    SCR_CONF_1      = 151,
    SCR_CONF_2      = 152,
    SCR_DETAILS     = 16,
    SCR_PRINTING    = 17,
    // Calibration flow
    SCR_LEVEL_MENU  = 31,
    SCR_LEVEL_WAIT  = 310,
    SCR_LEVEL_SKIP  = 32,
    SCR_LEVELLING   = 21,
    SCR_LEVEL_DONE  = 22,
    // File / test print
    SCR_FILE_MENU   = 41,
    SCR_FILE_SD     = 42,
    SCR_TEST_MENU   = 51,
    SCR_KEYCHAIN    = 52,
    // WiFi settings
    SCR_WIFI        = 60,
    // Utilities
    SCR_UTIL_MENU   = 201,
    SCR_CLEAN_EXP   = 2010,
    SCR_CLEAN_CONF  = 20101,
    SCR_TEST_CONF   = 2020,
    // Parameter edit screens (10011–10023 + 100231–100236)
    SCR_P_LAYER     = 10011,
    SCR_P_IEXPO     = 10012,
    SCR_P_EXPO      = 10013,
    SCR_P_UV        = 10014,
    SCR_P_FIRST     = 10015,
    SCR_P_TRANS     = 10016,
    SCR_P_HUPINIT  = 10017,
    SCR_P_HUP       = 10018,
    SCR_P_VINIT     = 10019,
    SCR_P_V         = 10020,
    SCR_P_VRET      = 10021,
    SCR_P_REST      = 10022,
    SCR_P_STORE     = 10023,
    SCR_P_STORE_A   = 100231,
    SCR_P_STORE_B   = 100232,
    SCR_P_STORE_C   = 100233,
    SCR_P_STORE_D   = 100234,
    SCR_P_STORE_E   = 100235,
    SCR_P_STORE_F   = 100236,
} screen_id_t;

static int   s_screen     = SCR_MAIN;
static int   s_pref_mode  = 0;   // 0=current, 1–6=A–F
static int   s_sel_pref   = 0;   // 0=from settings, 1=from print flow
static int   s_print_mode = 0;   // 0=SD, 1=keychain, 2=touch pencil
static int   s_clean_exp  = 45;
static int   s_layer_count = 0;
static bool  s_printing   = false;

// Folder list for file browser (fixes the O(N) scan-per-press bug)
#define MAX_FOLDERS 64
static char  s_folders[MAX_FOLDERS][25];
static int   s_folder_count = 0;
static int   s_folder_idx   = 0;

static int   s_layer_done  = 0;
static int   s_layer_total = 0;
static uint32_t s_elapsed_ms = 0;

// ── PNG draw helper ────────────────────────────────────────────────────────

static void draw_flash_png(const uint8_t *data, size_t len) {
    int rc = png.openFLASH((uint8_t *)data, len, png_draw_cb);
    if (rc == PNG_SUCCESS) {
        tft.startWrite();
        png.decode(nullptr, 0);
        tft.endWrite();
    }
}

// ── Number drawing helpers ─────────────────────────────────────────────────

static void draw_int(int x, int y, int v, uint8_t font, bool centre) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", v);
    if (centre) tft.drawCentreString(buf, x, y, font);
    else        tft.drawString(buf, x, y, font);
}

static void draw_float(int x, int y, float v, uint8_t font, bool centre) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", v);
    if (centre) tft.drawCentreString(buf, x, y, font);
    else        tft.drawString(buf, x, y, font);
}

// ── Screen rendering ───────────────────────────────────────────────────────

static void coord_rot3() { g_xpos = 27; g_ypos = 0; }
static void coord_rot2() { g_xpos = 0;  g_ypos = 27; }

static void set_landscape() { tft.setRotation(3); coord_rot3(); }
static void set_portrait()  { tft.setRotation(2); coord_rot2(); }

static void mask_left()  { tft.fillRect(0,   0, 77,  56, TFT_BLACK); }
static void mask_right() { tft.fillRect(379, 0, 60,  56, TFT_BLACK); }

static void scr_clear()  { tft.fillScreen(TFT_BLACK); }

static void scr_main() {
    draw_flash_png(menuprint, sizeof(menuprint));
}

static void scr_settings() {
    draw_flash_png(menusettings, sizeof(menusettings));
}

static void scr_utilities() {
    draw_flash_png(menuutilities, sizeof(menuutilities));
}

static void scr_wifi() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("WIFI",         240, 20, 4);

    char ssid[33] = ""; wifi_get_ssid(ssid, sizeof(ssid));
    char ip[16]   = ""; wifi_get_ip(ip,   sizeof(ip));

    switch (wifi_get_state()) {
        case WIFI_STATE_CONNECTED:
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawCentreString("CONNECTED",    240, 80, 4);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawCentreString(ssid,           240, 125, 2);
            tft.drawCentreString(ip,             240, 155, 4);
            tft.drawCentreString("lite3dp-g2.local", 240, 200, 2);
            break;
        case WIFI_STATE_CONNECTING:
            tft.setTextColor(TFT_YELLOW, TFT_BLACK);
            tft.drawCentreString("CONNECTING...", 240, 80, 4);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawCentreString(ssid,            240, 125, 2);
            break;
        case WIFI_STATE_AP_MODE:
            tft.setTextColor(TFT_CYAN, TFT_BLACK);
            tft.drawCentreString("SETUP MODE",   240, 80, 4);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawCentreString("Connect to:",  240, 120, 2);
            tft.drawCentreString("Lite3DP-G2",   240, 145, 4);
            tft.drawCentreString("Then open:",   240, 190, 2);
            tft.drawCentreString("192.168.4.1",  240, 215, 2);
            break;
        default:
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawCentreString("DISCONNECTED", 240, 80, 4);
            break;
    }

    // BACK hint at bottom
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("BACK = clear credentials", 240, 285, 2);
}

static void scr_tmpl1() { draw_flash_png(template1,   sizeof(template1));   }
static void scr_tmpl3() { draw_flash_png(template3,   sizeof(template3));   }
static void scr_tmpl2a(){ draw_flash_png(template2A,  sizeof(template2A));  }
static void scr_tmpl2b(){ draw_flash_png(template2B,  sizeof(template2B));  }

static void scr_file_sel() {
    scr_tmpl1();
    tft.setTextColor(TFT_WHITE);
    tft.drawCentreString("SELECT FILE", 240, 25, 4);
    if (s_folder_count > 0 && s_folder_idx < s_folder_count) {
        tft.drawCentreString(s_folders[s_folder_idx], 245, 150, 4);
    } else {
        tft.drawCentreString("(no folders)", 245, 150, 4);
    }
}

static void scr_param_sel() {
    scr_tmpl1();
    tft.setTextColor(TFT_WHITE);
    tft.drawCentreString("PARAMETERS", 240, 25, 4);
}

static void scr_param_slot_label(int slot) {
    tft.fillRect(120, 128, 240, 50, TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    const char *labels[] = { "Current parameters", "Change parameters",
                              "Memory A", "Memory B", "Memory C",
                              "Memory D", "Memory E", "Memory F" };
    tft.drawCentreString(labels[slot < 8 ? slot : 0], 245, 150, 4);
}

static void scr_conf_load() {
    draw_flash_png(template4, sizeof(template4));
    tft.setTextColor(TFT_WHITE);
    tft.drawCentreString("CONFIRMATION 1", 240, 25, 4);

    const print_params_t &p = g_params;
    int hum = (int)(p.hLayer * 1000.0f);
    tft.drawString("LAYER HEIGHT (um)",  70, 100, 4); draw_int(343, 100, hum,         4, false);
    tft.drawString("INITIAL EXPOS. (s)", 70, 154, 4); draw_int(343, 154, p.iExpoTime, 4, false);
    tft.drawString("EXPOSURE TIME (s)",  70, 208, 4); draw_float(343, 208, p.expoTime, 4, false);
    tft.drawString("UV POWER",           70, 262, 4); draw_int(343, 262, p.pwmUV,     4, false);
}

static void scr_conf1() {
    draw_flash_png(template4, sizeof(template4));
    tft.setTextColor(TFT_WHITE);
    tft.drawCentreString("CONFIRMATION 2", 240, 25, 4);

    const print_params_t &p = g_params;
    tft.drawString("BOTTOM LAYERS",       70, 100, 4); draw_int(343, 100, p.firstLayers, 4, false);
    tft.drawString("TRANS. LAYERS",       70, 154, 4); draw_int(343, 154, p.traLayers,   4, false);
    tft.drawString("B. LIFT HEIGHT(mm)",  70, 208, 4); draw_float(343, 208, p.hUpInitial, 4, false);
    tft.drawString("LIFT HEIGHT (mm)",    70, 262, 4); draw_float(343, 262, p.hUp,        4, false);
}

static void scr_conf2() {
    draw_flash_png(template4, sizeof(template4));
    tft.setTextColor(TFT_WHITE);
    tft.drawCentreString("CONFIRMATION 3", 240, 25, 4);

    const print_params_t &p = g_params;
    tft.drawString("LIFT SPEED (mm/s)",    70, 100, 4); draw_float(343, 100, p.liftSpeed,     4, false);
    tft.drawString("RETR. SPEED (mm/s)",   70, 154, 4); draw_float(343, 154, p.retractSpeed,  4, false);
    tft.drawString("B. LIFT SPEED (mm/s)", 70, 208, 4); draw_float(343, 208, p.liftSpeedInit, 4, false);
    tft.drawString("REST TIME (ms)",        70, 262, 4); draw_int(343, 262, g_params.restTime, 4, false);
}

static void scr_details() {
    scr_tmpl3();
    mask_right();
    set_landscape();
    tft.setTextColor(TFT_WHITE);
    tft.drawCentreString("DETAILS", 240, 25, 4);

    // Folder name (truncated to 9 chars + "..")
    tft.drawString("FOLDER:", 88, 110, 4);
    if (s_print_mode == 0 && s_folder_count > 0) {
        char short_name[12] = {};
        strncpy(short_name, s_folders[s_folder_idx], 9);
        strncat(short_name, "..", 2);
        tft.drawString(short_name, 215, 110, 4);
    } else if (s_print_mode == 1) {
        tft.drawString("Keychain", 230, 110, 4);
    } else {
        tft.drawString("Touch pencil", 230, 110, 4);
    }

    tft.drawString("TOTAL LAYERS:", 90, 155, 4);
    draw_int(310, 155, s_layer_count, 4, false);

    // Estimated time (simplified: expoTime × layers + lift/retract × layers)
    const print_params_t &p = g_params;
    float hDown = p.hUp - p.hLayer;
    float t_lift  = p.hUp   / p.liftSpeed;
    float t_ret   = hDown   / p.retractSpeed;
    float t_layer = p.expoTime + t_lift + (float)p.restTime / 1000.0f
                    + 0.2f + t_ret;
    float t_first = (float)p.iExpoTime + p.hUpInitial / p.liftSpeedInit
                    + (p.hUpInitial - p.hLayer) / p.retractSpeed + 0.2f;
    long total_s  = (long)(p.firstLayers * t_first
                            + (s_layer_count - p.firstLayers) * t_layer);
    int hrs = total_s / 3600;
    int min = (total_s % 3600) / 60;

    tft.drawString("EST. TIME:", 90, 200, 4);
    draw_int(257, 200, hrs, 4, false); tft.drawString("h",   272, 200, 4);
    draw_int(305, 200, min, 4, false); tft.drawString("min", 335, 200, 4);
    tft.drawCentreString("PRESS START!", 240, 260, 4);
}

static void scr_printing_progress() {
    set_landscape();
    draw_flash_png(progress, sizeof(progress));

    if (s_layer_total <= 0) return;

    int pct = 100 * s_layer_done / s_layer_total;
    int arc = s_layer_done * 360 / s_layer_total;

    if (arc <= 180) {
        tft.drawArc(244, 160, 120, 100, 180, 180 + arc, TFT_WHITE, TFT_WHITE);
    } else {
        tft.drawArc(244, 160, 120, 100, 180, arc - 180, TFT_WHITE, TFT_WHITE);
    }

    if (pct <= 99) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", pct);
        tft.drawCentreString(buf, 244, 85, 6);
        tft.setTextColor(TFT_YELLOW);
        tft.drawCentreString("%", 244, 130, 4);
        tft.setTextColor(TFT_WHITE);
    }

    // Remaining time estimate (rough: remaining layers × per-layer time)
    const print_params_t &p = g_params;
    int remaining_layers = s_layer_total - s_layer_done;
    float t_layer = p.expoTime + p.hUp / p.liftSpeed
                    + (p.hUp - p.hLayer) / p.retractSpeed + 0.2f;
    long rem_s = (long)(remaining_layers * t_layer);
    int hrs = rem_s / 3600;
    int min = (rem_s % 3600) / 60;

    tft.drawCentreString("TO FINISH:", 246, 175, 4);
    draw_int(187, 205, hrs, 4, false); tft.drawString("h",   202, 205, 4);
    draw_int(235, 205, min, 4, false); tft.drawString("min", 265, 205, 4);
}

static void scr_finished() {
    set_landscape();
    scr_clear();
    draw_flash_png(finished, sizeof(finished));

    long ms = (long)s_elapsed_ms;
    int hrs = ms / 3600000;
    int min = (ms % 3600000) / 60000;

    tft.drawCentreString("FINISHED IN:", 240, 220, 4);
    draw_int(172, 270, hrs, 4, false); tft.drawString("h",   202, 270, 4);
    draw_int(235, 270, min, 4, false); tft.drawString("min", 265, 270, 4);
}

static void scr_param_edit(int screen) {
    scr_tmpl1();
    tft.setTextColor(TFT_WHITE);
    print_params_t &p = g_params;
    int hum = (int)(p.hLayer * 1000.0f);

    switch (screen) {
        case SCR_P_LAYER:  tft.drawCentreString("1.LAYER HEIGHT",      240,25,4); draw_int(220,150,hum,4,true); tft.drawCentreString("um",270,150,4); break;
        case SCR_P_IEXPO:  tft.drawCentreString("2.INITIAL EXPOSURE",  240,25,4); draw_int(235,150,p.iExpoTime,4,true); tft.drawCentreString("s",280,150,4); break;
        case SCR_P_EXPO:   tft.drawCentreString("3.EXPOSURE TIME",     240,25,4); draw_float(207,150,p.expoTime,4,false); tft.drawCentreString("s",280,150,4); break;
        case SCR_P_UV:     tft.drawCentreString("4.UV POWER",          240,25,4); draw_int(240,150,p.pwmUV,4,true); tft.drawCentreString("(0-255)",240,212,4); break;
        case SCR_P_FIRST:  tft.drawCentreString("5.BOTTOM LAYER COUNT",240,25,4); draw_int(240,150,p.firstLayers,4,true); break;
        case SCR_P_TRANS:  tft.drawCentreString("6.TRANSITION LAYERS", 240,25,4); draw_int(240,150,p.traLayers,4,true); break;
        case SCR_P_HUPINIT:tft.drawCentreString("7.BOTTOM LIFT HEIGHT",240,25,4); draw_float(240,150,p.hUpInitial,4,true); tft.drawCentreString("(mm)",240,212,4); break;
        case SCR_P_HUP:    tft.drawCentreString("8.LIFT HEIGHT",       240,25,4); draw_float(240,150,p.hUp,4,true); tft.drawCentreString("(mm)",240,212,4); break;
        case SCR_P_VINIT:  tft.drawCentreString("9.BOTTOM LIFT SPEED", 240,25,4); draw_float(240,150,p.liftSpeedInit,4,true); tft.drawCentreString("(mm/s)",240,212,4); break;
        case SCR_P_V:      tft.drawCentreString("10.LIFT SPEED",       240,25,4); draw_float(240,150,p.liftSpeed,4,true); tft.drawCentreString("(mm/s)",240,212,4); break;
        case SCR_P_VRET:   tft.drawCentreString("11.RETRACT SPEED",    240,25,4); draw_float(240,150,p.retractSpeed,4,true); tft.drawCentreString("(mm/s)",240,212,4); break;
        case SCR_P_REST:   tft.drawCentreString("12.REST TIME",         240,25,4); draw_int(240,150,p.restTime,4,true); tft.drawCentreString("(ms)",240,212,4); break;
        case SCR_P_STORE:  tft.drawCentreString("13.STORE PARAMETERS", 240,25,4); tft.drawCentreString("DO NOT STORE",240,150,4); break;
        case SCR_P_STORE_A: tft.fillRect(120,128,240,45,TFT_BLACK); tft.drawCentreString("MEMORY A",240,150,4); break;
        case SCR_P_STORE_B: tft.fillRect(120,128,240,45,TFT_BLACK); tft.drawCentreString("MEMORY B",240,150,4); break;
        case SCR_P_STORE_C: tft.fillRect(120,128,240,45,TFT_BLACK); tft.drawCentreString("MEMORY C",240,150,4); break;
        case SCR_P_STORE_D: tft.fillRect(120,128,240,45,TFT_BLACK); tft.drawCentreString("MEMORY D",240,150,4); break;
        case SCR_P_STORE_E: tft.fillRect(120,128,240,45,TFT_BLACK); tft.drawCentreString("MEMORY E",240,150,4); break;
        case SCR_P_STORE_F: tft.fillRect(120,128,240,45,TFT_BLACK); tft.drawCentreString("MEMORY F",240,150,4); break;
    }
}

// ── Parameter editing ──────────────────────────────────────────────────────

static void param_increment(int screen) {
    print_params_t &p = g_params;
    int hum = (int)(p.hLayer * 1000.0f);
    switch (screen) {
        case SCR_P_LAYER:   if (hum < 100) { p.hLayer = (hum == 25) ? 0.05f : 0.10f; } break;
        case SCR_P_IEXPO:   if (p.iExpoTime < 999) p.iExpoTime++;   break;
        case SCR_P_EXPO:    if (p.expoTime < 999.8f) p.expoTime = roundf((p.expoTime + 0.2f) * 10.0f) / 10.0f; break;
        case SCR_P_UV:      if (p.pwmUV < 255) p.pwmUV++;          break;
        case SCR_P_FIRST:   if (p.firstLayers < 20) p.firstLayers++;  break;
        case SCR_P_TRANS:   if (p.traLayers < 11)   p.traLayers++;    break;
        case SCR_P_HUPINIT: if (p.hUpInitial < 14.9f) p.hUpInitial = roundf((p.hUpInitial + 0.1f) * 10.0f) / 10.0f; break;
        case SCR_P_HUP:     if (p.hUp < 15.0f)        p.hUp        = roundf((p.hUp + 0.1f)        * 10.0f) / 10.0f; break;
        case SCR_P_VINIT:   if (p.liftSpeedInit < 8.1f) p.liftSpeedInit = roundf((p.liftSpeedInit + 0.1f) * 10.0f) / 10.0f; break;
        case SCR_P_V:       if (p.liftSpeed < 8.1f)     p.liftSpeed     = roundf((p.liftSpeed + 0.1f)     * 10.0f) / 10.0f; break;
        case SCR_P_VRET:    if (p.retractSpeed < 8.0f)  p.retractSpeed  = roundf((p.retractSpeed + 0.1f)  * 10.0f) / 10.0f; break;
        case SCR_P_REST:    if (p.restTime < 30000) p.restTime += 100; break;
    }
}

static void param_decrement(int screen) {
    print_params_t &p = g_params;
    int hum = (int)(p.hLayer * 1000.0f);
    switch (screen) {
        case SCR_P_LAYER:   if (hum > 25) { p.hLayer = (hum == 100) ? 0.05f : 0.025f; } break;
        case SCR_P_IEXPO:   if (p.iExpoTime > 1) p.iExpoTime--;      break;
        case SCR_P_EXPO:    if (p.expoTime > 0.4f) p.expoTime = roundf((p.expoTime - 0.2f) * 10.0f) / 10.0f; break;
        case SCR_P_UV:      if (p.pwmUV > 0) p.pwmUV--;              break;
        case SCR_P_FIRST:   if (p.firstLayers > 1) p.firstLayers--;   break;
        case SCR_P_TRANS:   if (p.traLayers > 0)   p.traLayers--;     break;
        case SCR_P_HUPINIT: if (p.hUpInitial > 0.6f) p.hUpInitial = roundf((p.hUpInitial - 0.1f) * 10.0f) / 10.0f; break;
        case SCR_P_HUP:     if (p.hUp > 0.6f)        p.hUp        = roundf((p.hUp - 0.1f)        * 10.0f) / 10.0f; break;
        case SCR_P_VINIT:   if (p.liftSpeedInit > 0.1f) p.liftSpeedInit = roundf((p.liftSpeedInit - 0.1f) * 10.0f) / 10.0f; break;
        case SCR_P_V:       if (p.liftSpeed > 0.1f)     p.liftSpeed     = roundf((p.liftSpeed - 0.1f)     * 10.0f) / 10.0f; break;
        case SCR_P_VRET:    if (p.retractSpeed > 0.2f)  p.retractSpeed  = roundf((p.retractSpeed - 0.1f)  * 10.0f) / 10.0f; break;
        case SCR_P_REST:    if (p.restTime >= 100) p.restTime -= 100; break;
    }
}

// ── Touch → logical button mapping ────────────────────────────────────────

typedef enum { BTN_NONE, BTN_UP, BTN_DOWN, BTN_NEXT, BTN_BACK } btn_t;

static btn_t read_touch() {
    if (!ts.tirqTouched() || !ts.touched()) return BTN_NONE;
    TS_Point p = ts.getPoint();
    uint16_t xp = map(p.y, TS_MINX, TS_MAXX, 0, 320);
    uint16_t yp = map(p.x, TS_MINY, TS_MAXY, 0, 426);

    // Three-zone landscape layout used by most screens
    if (xp < 120)         return BTN_DOWN;
    if (xp > 150 && xp < 300) return BTN_NEXT;
    if (xp > 330)         return BTN_UP;
    return BTN_NONE;
}

// ── Compute total layers for the selected folder ───────────────────────────

static void refresh_layer_count() {
    if (s_print_mode == 0 && s_folder_count > 0) {
        s_layer_count = slicer_count_layers(s_folders[s_folder_idx]);
    } else if (s_print_mode == 1) {
        int hum = (int)(g_params.hLayer * 1000.0f);
        s_layer_count = (hum == 25) ? 144 : (hum == 50) ? 72 : 36;
    } else {
        int hum = (int)(g_params.hLayer * 1000.0f);
        s_layer_count = (hum == 25) ? 2480 : (hum == 50) ? 1240 : 620;
    }
}

// ── Send a print start command ─────────────────────────────────────────────

static void send_print_start() {
    refresh_layer_count();
    print_cmd_t cmd = {};
    cmd.type = CMD_PRINT_START;
    strncpy(cmd.start.folder, s_folders[s_folder_idx], sizeof(cmd.start.folder) - 1);
    cmd.start.layer_count = s_layer_count;
    cmd.start.print_mode  = s_print_mode;
    xQueueSend(g_print_cmd_queue, &cmd, portMAX_DELAY);
    s_printing = true;
}

// ── Navigation: NEXT button ────────────────────────────────────────────────

static void on_next() {
    switch (s_screen) {
        case SCR_MAIN:
            s_screen = SCR_LEVEL_MENU; scr_tmpl2a();
            tft.drawCentreString("LEVEL PLATFORM", 240, 25, 4);
            tft.drawCentreString("LEVEL", 240, 117, 4);
            tft.drawCentreString("SKIP",  240, 232, 4);
            break;
        case SCR_SETTINGS:
            s_sel_pref = 0; s_screen = SCR_P_LAYER; scr_param_edit(SCR_P_LAYER); break;
        case SCR_UTILITIES:
            s_screen = SCR_UTIL_MENU;
            scr_clear(); tft.setTextColor(TFT_WHITE);
            tft.drawCentreString("TOOLS",            240, 25,  4);
            tft.drawCentreString("CLEAN RESIN VAT",  240, 117, 4);
            tft.drawCentreString("FACTORY TEST",     240, 232, 4);
            scr_tmpl2a();
            break;
        case SCR_WIFI:
            // NEXT refreshes the WiFi status display
            scr_wifi();
            break;
        case SCR_FILE_SEL:
            refresh_layer_count();
            s_screen = SCR_PARAM_SEL; scr_param_sel(); break;
        case SCR_PARAM_SEL:
            s_pref_mode = 0; prefs_load(0, &g_params);
            s_screen = SCR_CONF_LOAD; scr_conf_load(); break;
        case SCR_PARAM_CHG:
            s_sel_pref = 1; s_screen = SCR_P_LAYER; scr_param_edit(SCR_P_LAYER); break;
        case SCR_PARAM_A: s_pref_mode=1; prefs_load(1,&g_params); s_screen=SCR_CONF_LOAD; scr_conf_load(); break;
        case SCR_PARAM_B: s_pref_mode=2; prefs_load(2,&g_params); s_screen=SCR_CONF_LOAD; scr_conf_load(); break;
        case SCR_PARAM_C: s_pref_mode=3; prefs_load(3,&g_params); s_screen=SCR_CONF_LOAD; scr_conf_load(); break;
        case SCR_PARAM_D: s_pref_mode=4; prefs_load(4,&g_params); s_screen=SCR_CONF_LOAD; scr_conf_load(); break;
        case SCR_PARAM_E: s_pref_mode=5; prefs_load(5,&g_params); s_screen=SCR_CONF_LOAD; scr_conf_load(); break;
        case SCR_PARAM_F: s_pref_mode=6; prefs_load(6,&g_params); s_screen=SCR_CONF_LOAD; scr_conf_load(); break;
        case SCR_CONF_LOAD: s_screen=SCR_CONF_1; scr_conf1();   break;
        case SCR_CONF_1:    s_screen=SCR_CONF_2; scr_conf2();   break;
        case SCR_CONF_2:    s_screen=SCR_DETAILS; scr_details(); break;
        case SCR_LEVEL_MENU:
            s_screen = SCR_LEVEL_WAIT;
            scr_tmpl3();
            tft.drawCentreString("LEVEL PLATFORM", 240, 25,  4);
            tft.drawCentreString("PLACE PLATFORM", 240, 140, 4);
            tft.drawCentreString("AND SPACER",     240, 180, 4);
            break;
        case SCR_LEVEL_WAIT: {
            scr_clear();
            scr_tmpl3(); mask_left(); mask_right();
            tft.drawCentreString("LEVELING...", 240, 25,  4);
            tft.drawCentreString("PLEASE WAIT", 240, 160, 4);
            s_screen = SCR_LEVELLING;
            print_cmd_t cmd = { .type = CMD_CALIBRATE };
            xQueueSend(g_print_cmd_queue, &cmd, 0);
            break;
        }
        case SCR_LEVEL_SKIP:
            s_screen = SCR_FILE_MENU;
            scr_clear(); tft.setTextColor(TFT_WHITE);
            tft.drawCentreString("FILE",        240, 25,  4);
            tft.drawCentreString("MICRO SD",    240, 117, 4);
            tft.drawCentreString("TEST PRINT",  240, 232, 4);
            scr_tmpl2a();
            break;
        case SCR_FILE_MENU:
            s_print_mode = 0;
            if (!sd_mount()) {
                scr_tmpl3(); tft.drawCentreString("SD CARD FAILURE", 240, 25, 4);
                tft.drawCentreString("Insert micro SD card.", 240, 140, 4);
                s_screen = SCR_LEVELLING; break;
            }
            s_folder_count = sd_list_folders(s_folders, MAX_FOLDERS);
            s_folder_idx = 0;
            s_screen = SCR_FILE_SEL; scr_file_sel();
            break;
        case SCR_TEST_MENU:
            s_screen = SCR_FILE_MENU;
            scr_clear(); tft.setTextColor(TFT_WHITE);
            tft.drawCentreString("TEST PRINT",   240, 25,  4);
            tft.drawCentreString("KEYCHAIN",     240, 117, 4);
            tft.drawCentreString("TOUCH PENCIL", 240, 232, 4);
            scr_tmpl2a();
            break;
        case SCR_UTIL_MENU:
            s_screen = SCR_CLEAN_EXP; scr_tmpl1();
            tft.drawCentreString("EXPOSURE TIME", 240, 25,  4);
            draw_int(240, 150, s_clean_exp, 4, true);
            break;
        case SCR_CLEAN_EXP: {
            scr_tmpl3(); mask_right();
            tft.drawCentreString("CLEAN RESIN VAT", 240, 25,  4);
            tft.drawString("EXPOSURE TIME (s):", 90, 125, 4);
            draw_int(340, 125, s_clean_exp, 4, false);
            tft.drawCentreString("PRESS START!", 240, 200, 4);
            s_screen = SCR_CLEAN_CONF;
            break;
        }
        case SCR_LEVEL_DONE: {
            // File selection after levelling
            s_screen = SCR_FILE_MENU;
            scr_clear(); tft.setTextColor(TFT_WHITE);
            tft.drawCentreString("FILE",       240, 25,  4);
            tft.drawCentreString("MICRO SD",   240, 117, 4);
            tft.drawCentreString("TEST PRINT", 240, 232, 4);
            scr_tmpl2a();
            break;
        }
        // Parameter edit: advance to next parameter
        case SCR_P_LAYER:  s_screen=SCR_P_IEXPO;  scr_param_edit(SCR_P_IEXPO);  break;
        case SCR_P_IEXPO:  s_screen=SCR_P_EXPO;   scr_param_edit(SCR_P_EXPO);   break;
        case SCR_P_EXPO:   s_screen=SCR_P_UV;     scr_param_edit(SCR_P_UV);     break;
        case SCR_P_UV:     s_screen=SCR_P_FIRST;  scr_param_edit(SCR_P_FIRST);  break;
        case SCR_P_FIRST:  s_screen=SCR_P_TRANS;  scr_param_edit(SCR_P_TRANS);  break;
        case SCR_P_TRANS:  s_screen=SCR_P_HUPINIT;scr_param_edit(SCR_P_HUPINIT);break;
        case SCR_P_HUPINIT:s_screen=SCR_P_HUP;    scr_param_edit(SCR_P_HUP);    break;
        case SCR_P_HUP:    s_screen=SCR_P_VINIT;  scr_param_edit(SCR_P_VINIT);  break;
        case SCR_P_VINIT:  s_screen=SCR_P_V;      scr_param_edit(SCR_P_V);      break;
        case SCR_P_V:      s_screen=SCR_P_VRET;   scr_param_edit(SCR_P_VRET);   break;
        case SCR_P_VRET:   s_screen=SCR_P_REST;   scr_param_edit(SCR_P_REST);   break;
        case SCR_P_REST:   s_screen=SCR_P_STORE;  scr_param_edit(SCR_P_STORE);  break;
        case SCR_P_STORE:
            prefs_save(0, &g_params);
            if (s_sel_pref == 0) { s_screen=SCR_MAIN; scr_main(); }
            else                  { s_screen=SCR_CONF_LOAD; scr_conf_load(); }
            break;
        case SCR_P_STORE_A: case SCR_P_STORE_B: case SCR_P_STORE_C:
        case SCR_P_STORE_D: case SCR_P_STORE_E: case SCR_P_STORE_F: {
            int slot = s_screen - SCR_P_STORE_A + 1;
            prefs_save(slot, &g_params);
            prefs_save(0, &g_params);
            if (s_sel_pref == 0) { s_screen=SCR_MAIN; scr_main(); }
            else                  { s_screen=SCR_CONF_LOAD; scr_conf_load(); }
            break;
        }
        default: break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ── Navigation: BACK button ────────────────────────────────────────────────

static void on_back() {
    switch (s_screen) {
        case SCR_SETTINGS:    s_screen=SCR_MAIN;        scr_main();       break;
        case SCR_UTILITIES:   s_screen=SCR_SETTINGS;    scr_settings();   break;
        case SCR_UTIL_MENU:   s_screen=SCR_UTILITIES;   scr_utilities();  break;
        case SCR_CLEAN_EXP:   s_screen=SCR_UTIL_MENU;
                              scr_clear(); tft.drawCentreString("TOOLS",240,25,4);
                              tft.drawCentreString("CLEAN RESIN VAT",240,117,4);
                              tft.drawCentreString("FACTORY TEST",240,232,4);
                              scr_tmpl2a(); break;
        case SCR_CLEAN_CONF:  s_screen=SCR_CLEAN_EXP;  scr_tmpl1();
                              tft.drawCentreString("EXPOSURE TIME",240,25,4);
                              draw_int(240,150,s_clean_exp,4,true); break;
        case SCR_FILE_SEL:    s_screen=SCR_FILE_MENU;
                              scr_clear(); scr_tmpl2a();
                              tft.drawCentreString("FILE",240,25,4);
                              tft.drawCentreString("MICRO SD",240,117,4);
                              tft.drawCentreString("TEST PRINT",240,232,4); break;
        case SCR_PARAM_SEL: case SCR_PARAM_CHG:
        case SCR_PARAM_A: case SCR_PARAM_B: case SCR_PARAM_C:
        case SCR_PARAM_D: case SCR_PARAM_E: case SCR_PARAM_F:
            if (s_print_mode != 0) { s_screen=SCR_FILE_MENU; }
            else                   { s_screen=SCR_FILE_SEL; scr_file_sel(); return; }
            scr_clear(); scr_tmpl2a();
            tft.drawCentreString("FILE",240,25,4);
            tft.drawCentreString("MICRO SD",240,117,4);
            tft.drawCentreString("TEST PRINT",240,232,4); break;
        case SCR_CONF_LOAD:   s_screen=SCR_PARAM_SEL;  scr_param_sel();  break;
        case SCR_CONF_1:      s_screen=SCR_CONF_LOAD;  scr_conf_load();  break;
        case SCR_CONF_2:      s_screen=SCR_CONF_1;     scr_conf1();      break;
        case SCR_DETAILS:     s_screen=SCR_CONF_2;     scr_conf2();      break;
        case SCR_PRINTING:    s_screen=SCR_MAIN;        scr_main();       break;
        case SCR_WIFI:
            // BACK from WiFi screen clears saved credentials → AP mode
            wifi_clear_credentials();
            s_screen=SCR_WIFI; scr_wifi();
            break;
        case SCR_LEVEL_MENU:  s_screen=SCR_MAIN;        scr_main();       break;
        case SCR_LEVEL_WAIT:  s_screen=SCR_LEVEL_MENU;
                              scr_tmpl2a();
                              tft.drawCentreString("LEVEL PLATFORM",240,25,4);
                              tft.drawCentreString("LEVEL",240,117,4);
                              tft.drawCentreString("SKIP",240,232,4); break;
        case SCR_FILE_MENU:   s_screen=SCR_LEVEL_SKIP;  scr_tmpl2b();    break;
        case SCR_TEST_MENU:   s_screen=SCR_FILE_MENU;
                              scr_clear(); scr_tmpl2a();
                              tft.drawCentreString("FILE",240,25,4);
                              tft.drawCentreString("MICRO SD",240,117,4);
                              tft.drawCentreString("TEST PRINT",240,232,4); break;
        case SCR_P_LAYER:
            prefs_save(0, &g_params);
            if (s_sel_pref==0) { s_screen=SCR_SETTINGS; scr_settings(); }
            else               { s_screen=SCR_CONF_LOAD; scr_conf_load(); }
            break;
        case SCR_P_IEXPO:  s_screen=SCR_P_LAYER;  scr_param_edit(SCR_P_LAYER);  break;
        case SCR_P_EXPO:   s_screen=SCR_P_IEXPO;  scr_param_edit(SCR_P_IEXPO);  break;
        case SCR_P_UV:     s_screen=SCR_P_EXPO;   scr_param_edit(SCR_P_EXPO);   break;
        case SCR_P_FIRST:  s_screen=SCR_P_UV;     scr_param_edit(SCR_P_UV);     break;
        case SCR_P_TRANS:  s_screen=SCR_P_FIRST;  scr_param_edit(SCR_P_FIRST);  break;
        case SCR_P_HUPINIT:s_screen=SCR_P_TRANS;  scr_param_edit(SCR_P_TRANS);  break;
        case SCR_P_HUP:    s_screen=SCR_P_HUPINIT;scr_param_edit(SCR_P_HUPINIT);break;
        case SCR_P_VINIT:  s_screen=SCR_P_HUP;    scr_param_edit(SCR_P_HUP);    break;
        case SCR_P_V:      s_screen=SCR_P_VINIT;  scr_param_edit(SCR_P_VINIT);  break;
        case SCR_P_VRET:   s_screen=SCR_P_V;      scr_param_edit(SCR_P_V);      break;
        case SCR_P_REST:   s_screen=SCR_P_VRET;   scr_param_edit(SCR_P_VRET);   break;
        case SCR_P_STORE:
        case SCR_P_STORE_A: case SCR_P_STORE_B: case SCR_P_STORE_C:
        case SCR_P_STORE_D: case SCR_P_STORE_E: case SCR_P_STORE_F:
            s_screen=SCR_P_REST; scr_param_edit(SCR_P_REST); break;
        default: break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ── Navigation: UP / DOWN (value change or list scroll) ───────────────────

static void on_up() {
    switch (s_screen) {
        case SCR_MAIN:      s_screen=SCR_SETTINGS;  scr_settings();  vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_SETTINGS:  s_screen=SCR_MAIN;      scr_main();      vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_UTILITIES: s_screen=SCR_SETTINGS;  scr_settings();  vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_WIFI:      s_screen=SCR_UTILITIES; scr_utilities(); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_FILE_SEL:
            if (s_folder_idx > 0) { s_folder_idx--; scr_file_sel(); }
            vTaskDelay(pdMS_TO_TICKS(200)); return;
        case SCR_CLEAN_EXP:
            if (s_clean_exp < 360) {
                s_clean_exp++;
                tft.fillRect(120, 128, 240, 45, TFT_BLACK);
                draw_int(240, 150, s_clean_exp, 4, true);
            }
            vTaskDelay(pdMS_TO_TICKS(100)); return;
        // Parameter UP = increment value
        case SCR_P_LAYER: case SCR_P_IEXPO: case SCR_P_EXPO: case SCR_P_UV:
        case SCR_P_FIRST: case SCR_P_TRANS: case SCR_P_HUPINIT: case SCR_P_HUP:
        case SCR_P_VINIT: case SCR_P_V:    case SCR_P_VRET:    case SCR_P_REST:
            param_increment(s_screen);
            scr_param_edit(s_screen);
            vTaskDelay(pdMS_TO_TICKS(80)); return;
        // Store slot selection
        case SCR_P_STORE_A: s_screen=SCR_P_STORE;  scr_param_edit(SCR_P_STORE);  vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_B: s_screen=SCR_P_STORE_A;scr_param_edit(SCR_P_STORE_A);vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_C: s_screen=SCR_P_STORE_B;scr_param_edit(SCR_P_STORE_B);vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_D: s_screen=SCR_P_STORE_C;scr_param_edit(SCR_P_STORE_C);vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_E: s_screen=SCR_P_STORE_D;scr_param_edit(SCR_P_STORE_D);vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_F: s_screen=SCR_P_STORE_E;scr_param_edit(SCR_P_STORE_E);vTaskDelay(pdMS_TO_TICKS(100)); return;
        // Parameter slot UP
        case SCR_PARAM_CHG: s_screen=SCR_PARAM_SEL; scr_param_sel(); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_A: s_screen=SCR_PARAM_CHG; scr_param_slot_label(1); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_B: s_screen=SCR_PARAM_A;   scr_param_slot_label(2); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_C: s_screen=SCR_PARAM_B;   scr_param_slot_label(3); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_D: s_screen=SCR_PARAM_C;   scr_param_slot_label(4); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_E: s_screen=SCR_PARAM_D;   scr_param_slot_label(5); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_F: s_screen=SCR_PARAM_E;   scr_param_slot_label(6); vTaskDelay(pdMS_TO_TICKS(100)); return;
        default: break;
    }
}

static void on_down() {
    switch (s_screen) {
        case SCR_SETTINGS:  s_screen=SCR_UTILITIES; scr_utilities(); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_UTILITIES: s_screen=SCR_WIFI;      scr_wifi();      vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_WIFI:      s_screen=SCR_SETTINGS;  scr_settings();  vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_FILE_SEL:
            if (s_folder_idx + 1 < s_folder_count) { s_folder_idx++; scr_file_sel(); }
            vTaskDelay(pdMS_TO_TICKS(200)); return;
        case SCR_CLEAN_EXP:
            if (s_clean_exp > 1) {
                s_clean_exp--;
                tft.fillRect(120, 128, 240, 45, TFT_BLACK);
                draw_int(240, 150, s_clean_exp, 4, true);
            }
            vTaskDelay(pdMS_TO_TICKS(100)); return;
        // Parameter DOWN = decrement value
        case SCR_P_LAYER: case SCR_P_IEXPO: case SCR_P_EXPO: case SCR_P_UV:
        case SCR_P_FIRST: case SCR_P_TRANS: case SCR_P_HUPINIT: case SCR_P_HUP:
        case SCR_P_VINIT: case SCR_P_V:    case SCR_P_VRET:    case SCR_P_REST:
            param_decrement(s_screen);
            scr_param_edit(s_screen);
            vTaskDelay(pdMS_TO_TICKS(80)); return;
        // Store slot DOWN
        case SCR_P_STORE:  s_screen=SCR_P_STORE_A; scr_param_edit(SCR_P_STORE_A); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_A:s_screen=SCR_P_STORE_B; scr_param_edit(SCR_P_STORE_B); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_B:s_screen=SCR_P_STORE_C; scr_param_edit(SCR_P_STORE_C); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_C:s_screen=SCR_P_STORE_D; scr_param_edit(SCR_P_STORE_D); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_D:s_screen=SCR_P_STORE_E; scr_param_edit(SCR_P_STORE_E); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_P_STORE_E:s_screen=SCR_P_STORE_F; scr_param_edit(SCR_P_STORE_F); vTaskDelay(pdMS_TO_TICKS(100)); return;
        // Parameter slot DOWN
        case SCR_PARAM_SEL: s_screen=SCR_PARAM_CHG; scr_param_slot_label(1); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_CHG: s_screen=SCR_PARAM_A;   scr_param_slot_label(2); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_A:   s_screen=SCR_PARAM_B;   scr_param_slot_label(3); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_B:   s_screen=SCR_PARAM_C;   scr_param_slot_label(4); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_C:   s_screen=SCR_PARAM_D;   scr_param_slot_label(5); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_D:   s_screen=SCR_PARAM_E;   scr_param_slot_label(6); vTaskDelay(pdMS_TO_TICKS(100)); return;
        case SCR_PARAM_E:   s_screen=SCR_PARAM_F;   scr_param_slot_label(7); vTaskDelay(pdMS_TO_TICKS(100)); return;
        default: break;
    }
}

// ── Play button ────────────────────────────────────────────────────────────

static void on_play() {
    switch (s_screen) {
        case SCR_MAIN:
            // Nudge platform up 0.05 mm (useful for manual positioning)
            { print_cmd_t c = { .type = CMD_MOVE_UP }; c.move.mm = 0.05f;
              xQueueSend(g_print_cmd_queue, &c, 0); }
            break;
        case SCR_DETAILS:
            scr_clear();
            send_print_start();
            s_screen = SCR_PRINTING;
            break;
        case SCR_CLEAN_CONF: {
            print_cmd_t c = { .type = CMD_CLEAN_VAT };
            c.clean.seconds = s_clean_exp;
            xQueueSend(g_print_cmd_queue, &c, 0);
            break;
        }
        case SCR_TEST_CONF: {
            print_cmd_t c = { .type = CMD_TEST_MODE };
            xQueueSend(g_print_cmd_queue, &c, 0);
            break;
        }
        case SCR_PRINTING:
            // Pause / resume
            if (s_printing) {
                print_cmd_t c = { .type = CMD_PRINT_PAUSE };
                xQueueSend(g_print_cmd_queue, &c, 0);
            }
            break;
        default: break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ── Consume status events from print_task ─────────────────────────────────

static void poll_status() {
    print_status_t st;
    while (xQueueReceive(g_print_status_queue, &st, 0) == pdTRUE) {
        switch (st.type) {
            case STATUS_LAYER_DONE:
                s_layer_done  = st.progress.layer;
                s_layer_total = st.progress.total;
                s_elapsed_ms  = st.progress.elapsed_ms;
                if (s_screen == SCR_PRINTING) scr_printing_progress();
                break;
            case STATUS_PRINT_COMPLETE:
                s_printing = false;
                s_elapsed_ms = st.progress.elapsed_ms;
                s_screen = SCR_PRINTING;
                scr_finished();
                break;
            case STATUS_PAUSED:
                if (s_screen == SCR_PRINTING) {
                    scr_tmpl3(); mask_left(); mask_right();
                    tft.drawCentreString("PAUSED",               240, 25,  4);
                    tft.drawCentreString("PRESS PLAY TO RESUME", 240, 150, 4);
                }
                break;
            case STATUS_CALIBRATED:
                s_screen = SCR_LEVEL_DONE;
                draw_flash_png(level, sizeof(level));
                break;
            case STATUS_CLEAN_DONE:
                s_screen = SCR_UTIL_MENU;
                scr_clear(); tft.setTextColor(TFT_WHITE);
                tft.drawCentreString("TOOLS",240,25,4);
                tft.drawCentreString("CLEAN RESIN VAT",240,117,4);
                tft.drawCentreString("FACTORY TEST",240,232,4);
                scr_tmpl2a();
                break;
            case STATUS_TEST_DONE:
                s_screen = SCR_MAIN; scr_main(); break;
            default: break;
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

void ui_init(void) {
    tft.begin();
    tft.setRotation(3);
    coord_rot3();
    ts.begin();

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BTN_PLAY),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    gpio_config_t det = {
        .pin_bit_mask = (1ULL << PIN_SD_DET),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&det);

    // Splash screen
    draw_flash_png(menuintro, sizeof(menuintro));
    tft.drawCentreString("V2.0-IDF", 240, 270, 4);
    vTaskDelay(pdMS_TO_TICKS(1200));
    scr_main();
}

void ui_task(void *arg) {
    ui_init();

    while (true) {
        // Check physical play button (active-LOW)
        if (gpio_get_level(PIN_BTN_PLAY) == 0) {
            on_play();
            while (gpio_get_level(PIN_BTN_PLAY) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Process touch input
        btn_t btn = read_touch();
        switch (btn) {
            case BTN_UP:   on_up();   break;
            case BTN_DOWN: on_down(); break;
            case BTN_NEXT: on_next(); break;
            case BTN_BACK: on_back(); break;
            default: break;
        }

        // Drain status queue from print_task
        poll_status();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
