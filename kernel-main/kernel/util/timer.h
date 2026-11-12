#ifndef KERNEL_UTIL_TIMER_H
#define KERNEL_UTIL_TIMER_H

#include <stdint.h>

void timer_init(void);

uint64_t timer_get_ms(void);

#endif
