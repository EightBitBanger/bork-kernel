#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>
#include <time.h>

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

/* The wall clock, like time() (time since boot on a machine without a
   clock chip, see time.h). tz is ignored; pass NULL. Resolution 1 ms. */
int gettimeofday(struct timeval* restrict tv, void* restrict tz);

#define timerisset(t)       ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t)       ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, CMP) \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP (b)->tv_usec) : ((a)->tv_sec CMP (b)->tv_sec))

#endif
