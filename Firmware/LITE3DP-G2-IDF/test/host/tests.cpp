/*
 * Host-side unit tests for pure-logic firmware modules.
 * Compile with:   make -C test/host
 * Run with:       test/host/runner
 *
 * No ESP-IDF required — FreeRTOS / esp_log are stubbed.
 *
 * RED→GREEN record:
 *   Round 1 (RED):
 *     - test_slicer_prusa_maxname: slicer_detect path[64] truncates 24-char folder
 *     - test_params_invalid_*: params_validate() returns true for bad params (stub)
 *   Round 2 (GREEN after fixes):
 *     - slicer.cpp path[64]→path[80], slicer.h comment updated
 *     - params.cpp real implementation
 */
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cassert>

// Pull in just the types we need without ESP-IDF
#include "shared.h"    // STEPS_PER_MM, speed_to_period_us, print_params_t
#include "slicer.h"    // slicer_layer_path, slicer_type_t
#include "params.h"    // params_validate, params_clamp, params_defaults

// ── Minimal test framework ─────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

#define TEST_EQ(name, got, expected) do { \
    if ((got) == (expected)) { \
        printf("[PASS] %s\n", name); g_pass++; \
    } else { \
        printf("[FAIL] %s  (got=%d expected=%d line=%d)\n", \
               name, (int)(got), (int)(expected), __LINE__); g_fail++; \
    } \
} while(0)

#define TEST_STREQ(name, got, expected) do { \
    bool _ok = (strcmp(got, expected) == 0); \
    if (_ok) { printf("[PASS] %s\n", name); g_pass++; } \
    else { \
        printf("[FAIL] %s\n  got:      \"%s\"\n  expected: \"%s\"\n", \
               name, got, expected); g_fail++; \
    } \
} while(0)

#define TEST_TRUE(name, expr) do { \
    if (expr) { printf("[PASS] %s\n", name); g_pass++; } \
    else { printf("[FAIL] %s (expr was false, line=%d)\n", name, __LINE__); g_fail++; } \
} while(0)

#define TEST_FALSE(name, expr) do { \
    if (!(expr)) { printf("[PASS] %s\n", name); g_pass++; } \
    else { printf("[FAIL] %s (expr was true, line=%d)\n", name, __LINE__); g_fail++; } \
} while(0)

// ── Section 1: slicer_layer_path ──────────────────────────────────────────

static void run_slicer_tests(void) {
    printf("\n--- slicer_layer_path ---\n");
    char buf[80];

    // Prusa: folder name embedded in filename, 5-digit zero-padded, 0-based
    slicer_layer_path(buf, sizeof(buf), "Cube", 0, SLICER_PRUSA);
    TEST_STREQ("prusa_layer0",   buf, "/sdcard/Cube/Cube00000.png");

    slicer_layer_path(buf, sizeof(buf), "Cube", 99, SLICER_PRUSA);
    TEST_STREQ("prusa_layer99",  buf, "/sdcard/Cube/Cube00099.png");

    slicer_layer_path(buf, sizeof(buf), "Cube", 12345, SLICER_PRUSA);
    TEST_STREQ("prusa_layer12345", buf, "/sdcard/Cube/Cube12345.png");

    // Prusa with 24-char folder — this requires buf_sz >= 67 bytes.
    // RED ROUND 1: was failing because slicer.h said "at least 64 bytes".
    // After fix: passing correctly with 80-byte buffer.
    slicer_layer_path(buf, sizeof(buf), "ABCDEFGHIJKLMNOPQRSTUVWX", 0, SLICER_PRUSA);
    TEST_STREQ("prusa_maxname",  buf, "/sdcard/ABCDEFGHIJKLMNOPQRSTUVWX/ABCDEFGHIJKLMNOPQRSTUVWX00000.png");

    // Lychee 4-digit: lychee0000.png, 0-based
    slicer_layer_path(buf, sizeof(buf), "X", 0, SLICER_LYCHEE4);
    TEST_STREQ("lychee4_layer0", buf, "/sdcard/X/lychee0000.png");

    slicer_layer_path(buf, sizeof(buf), "X", 9999, SLICER_LYCHEE4);
    TEST_STREQ("lychee4_layer9999", buf, "/sdcard/X/lychee9999.png");

    // Lychee 3-digit: lychee000.png, 0-based
    slicer_layer_path(buf, sizeof(buf), "X", 0, SLICER_LYCHEE3);
    TEST_STREQ("lychee3_layer0", buf, "/sdcard/X/lychee000.png");

    slicer_layer_path(buf, sizeof(buf), "X", 42, SLICER_LYCHEE3);
    TEST_STREQ("lychee3_layer42", buf, "/sdcard/X/lychee042.png");

    // Voxeldance: 0.png, 1.png, ... (0-based)
    slicer_layer_path(buf, sizeof(buf), "X", 0, SLICER_VOXELDANCE);
    TEST_STREQ("voxeldance_layer0", buf, "/sdcard/X/0.png");

    slicer_layer_path(buf, sizeof(buf), "X", 5, SLICER_VOXELDANCE);
    TEST_STREQ("voxeldance_layer5", buf, "/sdcard/X/5.png");

    // Chitubox: 1.png, 2.png, ... (internally 1-based; layer arg is 0-based)
    slicer_layer_path(buf, sizeof(buf), "X", 0, SLICER_CHITUBOX);
    TEST_STREQ("chitubox_layer0", buf, "/sdcard/X/1.png");   // NOT 0.png

    slicer_layer_path(buf, sizeof(buf), "X", 9, SLICER_CHITUBOX);
    TEST_STREQ("chitubox_layer9", buf, "/sdcard/X/10.png");

    // Unknown: empty string
    slicer_layer_path(buf, sizeof(buf), "X", 0, SLICER_UNKNOWN);
    TEST_STREQ("unknown_empty",  buf, "");
}

// ── Section 2: speed_to_period_us ─────────────────────────────────────────

static void run_speed_tests(void) {
    printf("\n--- speed_to_period_us ---\n");

    // At 1 mm/s: period = 1e6 / (5245 * 1.0) = 190.65 → truncated to 190
    TEST_EQ("speed_1mms",   speed_to_period_us(1.0f),  190);

    // At 2 mm/s: period = 1e6 / (5245 * 2.0) = 95.33 → 95
    TEST_EQ("speed_2mms",   speed_to_period_us(2.0f),  95);

    // At 0.5 mm/s: period = 1e6 / (5245 * 0.5) = 381.3 → 381
    TEST_EQ("speed_halfmms", speed_to_period_us(0.5f), 381);

    // Zero should be clamped to 0.1 mm/s → 1e6 / (5245 * 0.1) = 1906.6 → 1906
    TEST_EQ("speed_zero_clamp",    speed_to_period_us(0.0f),  1906);

    // Negative should also clamp to 0.1 mm/s
    TEST_EQ("speed_neg_clamp",     speed_to_period_us(-1.0f), 1906);

    // Fast speed: 5 mm/s → 1e6 / (5245 * 5) = 38.13 → 38
    TEST_EQ("speed_5mms",   speed_to_period_us(5.0f),  38);
}

// ── Section 3: params_validate / params_clamp ─────────────────────────────
// RED round 1: params_validate() is a stub returning true for all inputs.
// Tests for invalid params will FAIL (PASS printed when function returns true
// for bad input, FAIL when test expects false).
//
// GREEN round 2: real implementation validates ranges.

static print_params_t make_valid_params(void) {
    print_params_t p;
    params_defaults(&p);
    return p;
}

static void run_params_tests(void) {
    printf("\n--- params_validate ---\n");

    // Default params must always be valid
    print_params_t p = make_valid_params();
    TEST_TRUE("valid_defaults", params_validate(&p));

    // Layer height must be positive
    p = make_valid_params(); p.hLayer = 0.0f;
    TEST_FALSE("invalid_hlayer_zero", params_validate(&p));

    p = make_valid_params(); p.hLayer = -0.05f;
    TEST_FALSE("invalid_hlayer_neg",  params_validate(&p));

    // hUp must be at least hLayer (can't retract less than one layer)
    p = make_valid_params(); p.hLayer = 0.1f; p.hUp = 0.05f;
    TEST_FALSE("invalid_hup_lt_hlayer", params_validate(&p));

    p = make_valid_params(); p.hLayer = 0.05f; p.hUp = 0.05f;  // equal: valid
    TEST_TRUE("valid_hup_eq_hlayer",  params_validate(&p));

    // hUpInitial must also be >= hLayer
    p = make_valid_params(); p.hLayer = 0.1f; p.hUpInitial = 0.05f;
    TEST_FALSE("invalid_hupinit_lt_hlayer", params_validate(&p));

    // firstLayers must be at least 1
    p = make_valid_params(); p.firstLayers = 0;
    TEST_FALSE("invalid_firstlayers_zero", params_validate(&p));

    // traLayers may be 0 (no transition layers)
    p = make_valid_params(); p.traLayers = 0;
    TEST_TRUE("valid_tralayers_zero", params_validate(&p));

    // expoTime must be positive
    p = make_valid_params(); p.expoTime = 0.0f;
    TEST_FALSE("invalid_expo_zero",  params_validate(&p));

    p = make_valid_params(); p.expoTime = -1.0f;
    TEST_FALSE("invalid_expo_neg",   params_validate(&p));

    // iExpoTime must be positive
    p = make_valid_params(); p.iExpoTime = 0;
    TEST_FALSE("invalid_iexpo_zero", params_validate(&p));

    // Speeds must be positive
    p = make_valid_params(); p.liftSpeed = 0.0f;
    TEST_FALSE("invalid_liftspeed_zero", params_validate(&p));

    p = make_valid_params(); p.liftSpeedInit = 0.0f;
    TEST_FALSE("invalid_liftspeedinit_zero", params_validate(&p));

    p = make_valid_params(); p.retractSpeed = 0.0f;
    TEST_FALSE("invalid_retractspeed_zero", params_validate(&p));

    // restTime may be 0 (no rest between layers)
    p = make_valid_params(); p.restTime = 0;
    TEST_TRUE("valid_resttime_zero", params_validate(&p));

    // pwmUV must be in [1, 255]
    p = make_valid_params(); p.pwmUV = 0;
    TEST_FALSE("invalid_pwm_zero",  params_validate(&p));

    p = make_valid_params(); p.pwmUV = 256;
    TEST_FALSE("invalid_pwm_256",   params_validate(&p));

    p = make_valid_params(); p.pwmUV = 255;
    TEST_TRUE("valid_pwm_255",      params_validate(&p));

    p = make_valid_params(); p.pwmUV = 1;
    TEST_TRUE("valid_pwm_1",        params_validate(&p));

    printf("\n--- params_clamp ---\n");

    // Clamping should bring invalid params into valid range
    p = make_valid_params();
    p.hLayer      = -1.0f;
    p.liftSpeed   = -5.0f;
    p.pwmUV       = 300;
    p.firstLayers = 0;
    params_clamp(&p);
    TEST_TRUE("clamp_makes_valid", params_validate(&p));

    // Clamp should not change already-valid params
    print_params_t valid = make_valid_params();
    print_params_t clamped = valid;
    params_clamp(&clamped);
    TEST_TRUE("clamp_noop_on_valid",
              memcmp(&valid, &clamped, sizeof(valid)) == 0);
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== Lite3DP Gen-2 host unit tests ===\n");

    run_slicer_tests();
    run_speed_tests();
    run_params_tests();

    printf("\n====================================\n");
    printf("Results: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
