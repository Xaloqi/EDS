#ifndef MOCK_TIMER_H
#define MOCK_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Global mock tick counter (milliseconds) */
extern uint32_t mock_timer_ms;

void     mock_timer_reset(void);
void     mock_timer_advance_ms(uint32_t ms);
uint32_t mock_timer_now(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_TIMER_H */
