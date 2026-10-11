// Заглушка pico-sdk (pico/time.h): повторяющийся таймер — им кормится звук.
#pragma once
#include <stdint.h>

struct repeating_timer { int64_t delay_us; };
typedef struct repeating_timer repeating_timer_t;

bool add_repeating_timer_us(int64_t delay_us, bool (*cb)(repeating_timer_t *), void *user, repeating_timer_t *t);
bool cancel_repeating_timer(repeating_timer_t *t);
