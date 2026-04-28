#include "mock_timer.h"

uint32_t mock_timer_ms = 0u;

void mock_timer_reset(void)   { mock_timer_ms = 0u; }
void mock_timer_advance_ms(uint32_t ms) { mock_timer_ms += ms; }
uint32_t mock_timer_now(void) { return mock_timer_ms; }
