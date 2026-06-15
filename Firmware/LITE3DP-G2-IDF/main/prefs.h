#pragma once
#include "shared.h"

// NVS namespace names for the 7 parameter slots
#define PREF_NS_CURRENT  "Settings"
#define PREF_NS_A        "MemoryA"
#define PREF_NS_B        "MemoryB"
#define PREF_NS_C        "MemoryC"
#define PREF_NS_D        "MemoryD"
#define PREF_NS_E        "MemoryE"
#define PREF_NS_F        "MemoryF"

static const char* const PREF_NS[7] = {
    PREF_NS_CURRENT,
    PREF_NS_A, PREF_NS_B, PREF_NS_C,
    PREF_NS_D, PREF_NS_E, PREF_NS_F,
};

void prefs_init(void);
void prefs_load(int slot, print_params_t *p);   // slot 0 = current, 1–6 = A–F
void prefs_save(int slot, const print_params_t *p);
