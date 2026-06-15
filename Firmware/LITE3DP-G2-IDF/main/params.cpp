#include "params.h"
#include <string.h>

// ── Valid ranges ───────────────────────────────────────────────────────────

#define HLAYER_MIN      0.025f
#define HLAYER_MAX      0.200f
#define HUP_MAX         ((float)MAX_HEIGHT_MM)
#define FIRST_MIN       1
#define FIRST_MAX       20
#define TRA_MIN         0
#define TRA_MAX         10
#define EXPO_MIN        0.5f
#define EXPO_MAX        300.0f
#define IEXPO_MIN       1
#define IEXPO_MAX       999
#define SPEED_MIN       0.1f
#define SPEED_MAX       10.0f
#define REST_MIN        0
#define REST_MAX        10000
#define STEPS_ADD_MIN   (-500L)
#define STEPS_ADD_MAX   ( 500L)

// ── Public API ─────────────────────────────────────────────────────────────

void params_defaults(print_params_t *p) {
    p->hLayer         = 0.05f;
    p->hUp            = 5.0f;
    p->hUpInitial     = 7.0f;
    p->firstLayers    = 4;
    p->traLayers      = 4;
    p->expoTime       = 8.0f;
    p->iExpoTime      = 45;
    p->liftSpeed      = 2.5f;
    p->liftSpeedInit  = 1.5f;
    p->retractSpeed   = 3.0f;
    p->restTime       = 500;
    p->pwmUV          = 255;
    p->stepsAdditional = 0;
}

bool params_validate(const print_params_t *p) {
    if (p->hLayer < HLAYER_MIN || p->hLayer > HLAYER_MAX) return false;
    if (p->hUp < p->hLayer    || p->hUp > HUP_MAX)        return false;
    if (p->hUpInitial < p->hLayer || p->hUpInitial > HUP_MAX) return false;
    if (p->firstLayers < FIRST_MIN || p->firstLayers > FIRST_MAX) return false;
    if (p->traLayers   < TRA_MIN   || p->traLayers   > TRA_MAX)   return false;
    if (p->expoTime  < EXPO_MIN  || p->expoTime  > EXPO_MAX)  return false;
    if (p->iExpoTime < IEXPO_MIN || p->iExpoTime > IEXPO_MAX) return false;
    if (p->liftSpeed     < SPEED_MIN || p->liftSpeed     > SPEED_MAX) return false;
    if (p->liftSpeedInit < SPEED_MIN || p->liftSpeedInit > SPEED_MAX) return false;
    if (p->retractSpeed  < SPEED_MIN || p->retractSpeed  > SPEED_MAX) return false;
    if (p->restTime < REST_MIN || p->restTime > REST_MAX) return false;
    if (p->pwmUV < 1 || p->pwmUV > 255) return false;
    if (p->stepsAdditional < STEPS_ADD_MIN || p->stepsAdditional > STEPS_ADD_MAX) return false;
    return true;
}

#define CLAMPF(v, lo, hi)  ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define CLAMPI(v, lo, hi)  ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

void params_clamp(print_params_t *p) {
    p->hLayer         = CLAMPF(p->hLayer,         HLAYER_MIN, HLAYER_MAX);
    // hUp and hUpInitial depend on hLayer, so clamp after hLayer is fixed
    p->hUp            = CLAMPF(p->hUp,            p->hLayer,  HUP_MAX);
    p->hUpInitial     = CLAMPF(p->hUpInitial,     p->hLayer,  HUP_MAX);
    p->firstLayers    = CLAMPI(p->firstLayers,    FIRST_MIN,  FIRST_MAX);
    p->traLayers      = CLAMPI(p->traLayers,      TRA_MIN,    TRA_MAX);
    p->expoTime       = CLAMPF(p->expoTime,       EXPO_MIN,   EXPO_MAX);
    p->iExpoTime      = CLAMPI(p->iExpoTime,      IEXPO_MIN,  IEXPO_MAX);
    p->liftSpeed      = CLAMPF(p->liftSpeed,      SPEED_MIN,  SPEED_MAX);
    p->liftSpeedInit  = CLAMPF(p->liftSpeedInit,  SPEED_MIN,  SPEED_MAX);
    p->retractSpeed   = CLAMPF(p->retractSpeed,   SPEED_MIN,  SPEED_MAX);
    p->restTime       = CLAMPI(p->restTime,       REST_MIN,   REST_MAX);
    p->pwmUV          = CLAMPI(p->pwmUV,          1,          255);
    p->stepsAdditional = CLAMPI(p->stepsAdditional, STEPS_ADD_MIN, STEPS_ADD_MAX);
}
