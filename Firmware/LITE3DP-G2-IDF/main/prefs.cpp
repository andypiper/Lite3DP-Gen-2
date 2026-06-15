#include "prefs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "prefs";

void prefs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// ── Helpers ───────────────────────────────────────────────────────────────

static void nvs_get_f(nvs_handle_t h, const char *key, float *out, float def) {
    uint32_t raw = 0;
    if (nvs_get_u32(h, key, &raw) == ESP_OK) {
        memcpy(out, &raw, sizeof(float));
    } else {
        *out = def;
    }
}

static void nvs_set_f(nvs_handle_t h, const char *key, float val) {
    uint32_t raw;
    memcpy(&raw, &val, sizeof(float));
    nvs_set_u32(h, key, raw);
}

// ── Public API ────────────────────────────────────────────────────────────

void prefs_load(int slot, print_params_t *p) {
    if (slot < 0 || slot > 6) slot = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(PREF_NS[slot], NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "namespace '%s' not found, using defaults", PREF_NS[slot]);
        // Defaults match the original firmware
        p->hLayer        = 0.1f;
        p->hUp           = 4.0f;
        p->hUpInitial    = 6.0f;
        p->firstLayers   = 4;
        p->traLayers     = 0;
        p->expoTime      = 6.0f;
        p->iExpoTime     = 40;
        p->liftSpeed     = 1.7f;
        p->liftSpeedInit = 0.7f;
        p->retractSpeed  = 2.9f;
        p->restTime      = 0;
        p->pwmUV         = 255;
        p->stepsAdditional = 0;
        return;
    }

    nvs_get_f(h, "hLayer",        &p->hLayer,        0.1f);
    nvs_get_f(h, "hUp",           &p->hUp,           4.0f);
    nvs_get_f(h, "hUpInitial",    &p->hUpInitial,    6.0f);
    nvs_get_i32(h, "FirstLayers", &p->firstLayers,   4);
    nvs_get_i32(h, "TraLayers",   &p->traLayers,     0);
    nvs_get_f(h, "expotime",      &p->expoTime,      6.0f);
    nvs_get_i32(h, "iexpotime",   &p->iExpoTime,     40);
    nvs_get_f(h, "LiftSpeed",     &p->liftSpeed,     1.7f);
    nvs_get_f(h, "LiftSpeedInit", &p->liftSpeedInit, 0.7f);
    nvs_get_f(h, "RetractSpeed",  &p->retractSpeed,  2.9f);
    nvs_get_i32(h, "RestTime",    &p->restTime,      0);
    nvs_get_i32(h, "PWMUV",       &p->pwmUV,         255);

    int32_t sa = 0;
    nvs_get_i32(h, "stepsadd",    &sa,               0);
    p->stepsAdditional = (long)sa;

    nvs_close(h);
}

void prefs_save(int slot, const print_params_t *p) {
    if (slot < 0 || slot > 6) slot = 0;
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(PREF_NS[slot], NVS_READWRITE, &h));

    nvs_set_f(h, "hLayer",       p->hLayer);
    nvs_set_f(h, "hUp",          p->hUp);
    nvs_set_f(h, "hUpInitial",   p->hUpInitial);
    nvs_set_i32(h, "FirstLayers", p->firstLayers);
    nvs_set_i32(h, "TraLayers",   p->traLayers);
    nvs_set_f(h, "expotime",     p->expoTime);
    nvs_set_i32(h, "iexpotime",  p->iExpoTime);
    nvs_set_f(h, "LiftSpeed",    p->liftSpeed);
    nvs_set_f(h, "LiftSpeedInit", p->liftSpeedInit);
    nvs_set_f(h, "RetractSpeed", p->retractSpeed);
    nvs_set_i32(h, "RestTime",   p->restTime);
    nvs_set_i32(h, "PWMUV",      p->pwmUV);
    nvs_set_i32(h, "stepsadd",   (int32_t)p->stepsAdditional);

    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}
