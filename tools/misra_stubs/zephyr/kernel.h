/* MISRA analysis stub for <zephyr/kernel.h> (EDS#583). Declarations only. */
#ifndef MISRA_STUB_ZEPHYR_KERNEL_H
#define MISRA_STUB_ZEPHYR_KERNEL_H

#include <stdint.h>

struct k_sem { unsigned int count; unsigned int limit; };
typedef struct { int64_t ticks; } k_timeout_t;

#define K_FOREVER           ((k_timeout_t){ -1 })
#define K_SEM_DEFINE(name, initial, limit)  struct k_sem name = { (initial), (limit) }
#define K_PRIO_PREEMPT(x)   (x)
#define K_THREAD_DEFINE(name, stack, entry, p1, p2, p3, prio, opts, delay) \
    extern int name

int32_t k_msleep(int32_t ms);
int     k_sem_take(struct k_sem *sem, k_timeout_t timeout);
void    k_sem_give(struct k_sem *sem);

#endif
