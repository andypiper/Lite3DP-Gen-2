#pragma once
#include "shared.h"
#include <stdbool.h>

// Populate p with factory-default print parameters.
void params_defaults(print_params_t *p);

// Return true if all fields of p are within acceptable ranges.
bool params_validate(const print_params_t *p);

// Clamp every field of p to the nearest valid value (idempotent on valid input).
void params_clamp(print_params_t *p);
