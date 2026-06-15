#include "motor.h"
#include "pins.h"
#include "shared.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include <cmath>

static const char *TAG = "motor";

// ── ISR context ───────────────────────────────────────────────────────────

typedef struct {
    volatile long steps_remaining;
    volatile bool step_high;
    SemaphoreHandle_t done_sem;
} stepper_ctx_t;

static gptimer_handle_t s_timer = NULL;
static stepper_ctx_t    s_ctx   = {};

// Toggles step pin every half-period; signals semaphore after final falling edge.
static bool IRAM_ATTR stepper_isr(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t *edata,
                                   void *user_data) {
    stepper_ctx_t *ctx = (stepper_ctx_t *)user_data;
    BaseType_t hp_woken = pdFALSE;

    ctx->step_high = !ctx->step_high;
    gpio_set_level(PIN_STEP, ctx->step_high ? 1 : 0);

    if (!ctx->step_high) {                  // falling edge = one complete step
        if (--ctx->steps_remaining == 0) {
            gptimer_stop(timer);
            xSemaphoreGiveFromISR(ctx->done_sem, &hp_woken);
        }
    }

    return hp_woken == pdTRUE;
}

// ── Public API ────────────────────────────────────────────────────────────

void motor_init(void) {
    // Configure output pins
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_DIR) | (1ULL << PIN_STEP) | (1ULL << PIN_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    // Configure endstop input
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_ENDSTOP),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    motor_enable(false);
    gpio_set_level(PIN_STEP, 0);
    gpio_set_level(PIN_DIR,  0);

    s_ctx.done_sem = xSemaphoreCreateBinary();
    configASSERT(s_ctx.done_sem);

    // Create 1 MHz timer (1 µs per tick)
    gptimer_config_t tcfg = {
        .clk_src        = GPTIMER_CLK_SRC_DEFAULT,
        .direction      = GPTIMER_COUNT_UP,
        .resolution_hz  = 1000000,
        .intr_priority  = 0,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&tcfg, &s_timer));

    gptimer_event_callbacks_t cbs = { .on_alarm = stepper_isr };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cbs, &s_ctx));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));

    ESP_LOGI(TAG, "init ok, %d steps/mm", STEPS_PER_MM);
}

void motor_enable(bool en) {
    // DRV8825 ENABLE: LOW = enabled
    gpio_set_level(PIN_EN, en ? 0 : 1);
}

void motor_move_steps(bool up, long steps, int period_us) {
    if (steps <= 0) return;

    gpio_set_level(PIN_DIR, up ? 1 : 0);
    motor_enable(true);

    // DRV8825 minimum STEP pulse width: 1.9 µs → half-period ≥ 2 µs
    uint64_t half_us = (uint64_t)(period_us / 2);
    if (half_us < 2) half_us = 2;

    s_ctx.steps_remaining = steps;
    s_ctx.step_high       = false;
    xSemaphoreTake(s_ctx.done_sem, 0);     // clear any stale token

    gptimer_alarm_config_t alarm = {
        .alarm_count             = half_us,
        .reload_count            = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer, &alarm));
    ESP_ERROR_CHECK(gptimer_set_raw_count(s_timer, 0));
    ESP_ERROR_CHECK(gptimer_start(s_timer));

    // Motor task (Core 1) blocks here until ISR fires the semaphore
    xSemaphoreTake(s_ctx.done_sem, portMAX_DELAY);
}

void motor_move_mm(bool up, float mm, int period_us) {
    long steps = (long)(STEPS_PER_MM * mm);
    motor_move_steps(up, steps, period_us);
}

void motor_home(int period_us) {
    // Safe maximum: guide length + 20 % margin
    int max_steps = (int)(LGUIDE_MM * STEPS_PER_MM * 1.2f);

    gpio_set_level(PIN_DIR, 0);     // descend
    motor_enable(true);

    int half_us = period_us / 2;
    if (half_us < 2) half_us = 2;

    for (int i = 0; i < max_steps; i++) {
        if (gpio_get_level(PIN_ENDSTOP) == 1) break;

        gpio_set_level(PIN_STEP, 1);
        esp_rom_delay_us(half_us);
        gpio_set_level(PIN_STEP, 0);
        esp_rom_delay_us(half_us);

        // Feed watchdog every 256 steps (~26 ms at default home speed)
        if ((i & 0xFF) == 0) {
            esp_task_wdt_reset();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(300));     // allow endstop to settle
}

bool motor_endstop(void) {
    return gpio_get_level(PIN_ENDSTOP) == 1;
}
