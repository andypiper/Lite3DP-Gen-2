#pragma once
#include <stdbool.h>

void motor_init(void);
void motor_enable(bool en);

// Execute a move: blocks until complete (waits on binary semaphore from ISR).
// period_us is the full step period; half-period used as timer interval.
void motor_move_steps(bool up, long steps, int period_us);

void motor_move_mm(bool up, float mm, int period_us);

// Descend until endstop triggers or max_steps reached.
void motor_home(int period_us);

bool motor_endstop(void);
