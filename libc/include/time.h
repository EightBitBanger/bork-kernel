#ifndef _TIME_H
#define _TIME_H

/*
 * Two clocks. CLOCK_MONOTONIC (and CLOCK_BOOTTIME, the same clock) is time
 * since boot from the kernel's millisecond tick (SYS_TIME): it never goes
 * back and is never set. CLOCK_REALTIME, time() and gettimeofday() are the
 * wall clock (SYS_CLOCK): the date and time the machine's clock chip keeps
 * (the CMOS clock on a PC), counted from 1970-01-01 00:00:00.
 *
 * On a machine without a clock chip the wall clock is uptime as if boot
 * were the Unix epoch, so dates come out as 1 January 1970 plus uptime
 * (os_clock_ms() in os/os.h says which you have).
 *
 * There are no time zones: the chip's time is taken as it is, and local
 * time is the same as UTC. A PC whose firmware clock keeps local time (as
 * Windows sets it) shows local time everywhere.
 *
 *     struct timespec t;
 *     clock_gettime(CLOCK_MONOTONIC, &t);     // resolution 1 ms
 *
 *     time_t now = time(NULL);
 *     struct tm tm;
 *     localtime_r(&now, &tm);                 // tm.tm_hour, tm.tm_min ...
 */

#include <stddef.h>
#include <sys/types.h>

#define CLOCKS_PER_SEC      1000L

#define CLOCK_REALTIME      0
#define CLOCK_MONOTONIC     1
#define CLOCK_BOOTTIME      7

typedef long    clock_t;
typedef int     clockid_t;

struct tm {
    int tm_sec;     /* 0-60 */
    int tm_min;     /* 0-59 */
    int tm_hour;    /* 0-23 */
    int tm_mday;    /* 1-31 */
    int tm_mon;     /* 0-11 */
    int tm_year;    /* Years since 1900 */
    int tm_wday;    /* 0-6, Sunday = 0 */
    int tm_yday;    /* 0-365 */
    int tm_isdst;   /* Always 0 */
};

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

// CLOCK_MONOTONIC / CLOCK_BOOTTIME: time since boot. CLOCK_REALTIME: the
// wall clock (see above). 0, or -1 with errno EINVAL (another clock), EFAULT.
int clock_gettime(clockid_t clock, struct timespec* out);
int clock_getres(clockid_t clock, struct timespec* out);     // 1 ms

// Set CLOCK_REALTIME (os_clock_set in os/os.h: needs admin, years
// 2000-2099). 0, or -1 with errno EPERM (not admin, or another clock),
// EINVAL, EFAULT, ENOENT (no clock chip), EIO.
int clock_settime(clockid_t clock, const struct timespec* t);

// Milliseconds since boot in CLOCKS_PER_SEC units (no per-process CPU time
// yet). Wraps after about 24 days on this 32-bit clock_t.
clock_t clock(void);

// Sleep at least *req (rounded up to whole milliseconds). Never
// interrupted, so *rem (may be NULL) is set to zero. EINVAL for a negative
// or out-of-range request.
int nanosleep(const struct timespec* req, struct timespec* rem);

// Seconds since 1970 on the wall clock (since boot without one, see above).
// Also stored in *out unless it is NULL.
time_t time(time_t* out);
double difftime(time_t end, time_t start);

// Broken-down time, UTC. The plain versions return a static struct that the
// next call overwrites; the _r versions fill *out. NULL with errno
// EOVERFLOW if the year does not fit an int.
struct tm* gmtime(const time_t* t);
struct tm* gmtime_r(const time_t* restrict t, struct tm* restrict out);
struct tm* localtime(const time_t* t);
struct tm* localtime_r(const time_t* restrict t, struct tm* restrict out);

// The reverse: normalizes out-of-range fields in *tm (e.g. tm_mday 32) and
// fills tm_wday/tm_yday. mktime and timegm agree, as local time is UTC.
time_t mktime(struct tm* tm);
time_t timegm(struct tm* tm);

#endif
