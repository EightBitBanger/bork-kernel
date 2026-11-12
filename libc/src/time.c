/*
 * time.h, gettimeofday(), os_time_ms() on SYS_TIME (milliseconds since
 * boot, 64-bit) and os_clock_ms() on SYS_CLOCK (the wall clock, milliseconds
 * since 1970). Without a wall clock, the calendar functions treat boot as
 * the Unix epoch.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#include <os/os.h>
#include "internal.h"

uint64_t os_time_ms(void) {
    uint64_t ms = 0;
    if (SYS_IS_ERROR(syscall1(SYS_TIME, (uint32_t)(uintptr_t)&ms)))
        return 0;
    return ms;
}

int os_clock_ms(uint64_t* out) {
    uint64_t ms = 0;
    if (__sys_ret(syscall1(SYS_CLOCK, (uint32_t)(uintptr_t)&ms)) < 0)
        return -1;
    if (out != NULL)
        *out = ms;
    return 0;
}

int os_clock_set(uint64_t ms) {
    return __sys_ret(syscall1(SYS_CLOCK_SET, (uint32_t)(uintptr_t)&ms)) < 0 ? -1 : 0;
}

int clock_settime(clockid_t clock, const struct timespec* t) {
    if (clock != CLOCK_REALTIME) {
        errno = (clock == CLOCK_MONOTONIC || clock == CLOCK_BOOTTIME) ? EPERM : EINVAL;
        return -1;
    }
    if (t == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (t->tv_sec < 0 || t->tv_nsec < 0 || t->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    return os_clock_set((uint64_t)t->tv_sec * 1000u + (uint64_t)(t->tv_nsec / 1000000L));
}

// The wall clock, or uptime as if boot were 1970 when there is none
static uint64_t realtime_ms(void) {
    uint64_t ms;
    if (syscall1(SYS_CLOCK, (uint32_t)(uintptr_t)&ms) == SYS_OK)
        return ms;
    return os_time_ms();
}

static bool known_clock(clockid_t clock) {
    return clock == CLOCK_REALTIME || clock == CLOCK_MONOTONIC || clock == CLOCK_BOOTTIME;
}

int clock_gettime(clockid_t clock, struct timespec* out) {
    if (!known_clock(clock)) {
        errno = EINVAL;
        return -1;
    }
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }
    uint64_t ms = 0;
    if (clock == CLOCK_REALTIME) {
        ms = realtime_ms();
    } else {
        int r = __sys_ret(syscall1(SYS_TIME, (uint32_t)(uintptr_t)&ms));
        if (r < 0)
            return -1;
    }
    out->tv_sec  = (time_t)(ms / 1000u);
    out->tv_nsec = (long)(ms % 1000u) * 1000000L;
    return 0;
}

int clock_getres(clockid_t clock, struct timespec* out) {
    if (!known_clock(clock)) {
        errno = EINVAL;
        return -1;
    }
    if (out != NULL) {
        out->tv_sec  = 0;
        out->tv_nsec = 1000000L;
    }
    return 0;
}

clock_t clock(void) {
    return (clock_t)os_time_ms();
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    // Whole milliseconds, rounded up, in slices that fit SYS_SLEEP's 32 bits
    uint64_t ms = (uint64_t)req->tv_sec * 1000u + ((uint64_t)req->tv_nsec + 999999u) / 1000000u;
    while (ms > 0) {
        uint32_t slice = (ms > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (uint32_t)ms;
        if (__sys_ret(syscall1(SYS_SLEEP, slice)) < 0)
            return -1;
        ms -= slice;
    }
    if (rem != NULL) {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

//
// Calendar
//

time_t time(time_t* out) {
    time_t t = (time_t)(realtime_ms() / 1000u);
    if (out != NULL)
        *out = t;
    return t;
}

double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

int gettimeofday(struct timeval* restrict tv, void* restrict tz) {
    (void)tz;
    if (tv == NULL) {
        errno = EFAULT;
        return -1;
    }
    uint64_t ms = realtime_ms();
    tv->tv_sec  = (time_t)(ms / 1000u);
    tv->tv_usec = (suseconds_t)(ms % 1000u) * 1000;
    return 0;
}

// Days since 1970-01-01 <-> proleptic Gregorian date (H. Hinnant's
// algorithms; eras of 400 years make the leap rules exact)
static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= (m <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long* y, unsigned* m, unsigned* d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (long long)yoe + era * 400 + (*m <= 2);
}

struct tm* gmtime_r(const time_t* restrict t, struct tm* restrict out) {
    if (t == NULL || out == NULL) {
        errno = EFAULT;
        return NULL;
    }
    long long days = *t / 86400;
    long long secs = *t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }

    long long year;
    unsigned mon, mday;
    civil_from_days(days, &year, &mon, &mday);
    if (year - 1900 > INT_MAX || year - 1900 < INT_MIN) {
        errno = EOVERFLOW;
        return NULL;
    }

    out->tm_sec   = (int)(secs % 60);
    out->tm_min   = (int)(secs / 60 % 60);
    out->tm_hour  = (int)(secs / 3600);
    out->tm_mday  = (int)mday;
    out->tm_mon   = (int)mon - 1;
    out->tm_year  = (int)(year - 1900);
    out->tm_wday  = (int)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday
    out->tm_yday  = (int)(days - days_from_civil(year, 1, 1));
    out->tm_isdst = 0;
    return out;
}

struct tm* gmtime(const time_t* t) {
    static struct tm result;
    return gmtime_r(t, &result);
}

struct tm* localtime_r(const time_t* restrict t, struct tm* restrict out) {
    return gmtime_r(t, out);    // No time zones
}

struct tm* localtime(const time_t* t) {
    return gmtime(t);
}

time_t timegm(struct tm* tm) {
    // Carry months into years first, then let the day count absorb the rest
    long long year = (long long)tm->tm_year + 1900;
    long long mon  = tm->tm_mon;
    year += mon / 12;
    mon  %= 12;
    if (mon < 0) {
        mon += 12;
        year--;
    }

    long long days = days_from_civil(year, (unsigned)mon + 1, 1) + (long long)tm->tm_mday - 1;
    time_t t = (time_t)days * 86400 + (time_t)tm->tm_hour * 3600 +
               (time_t)tm->tm_min * 60 + (time_t)tm->tm_sec;

    struct tm normalized;
    if (gmtime_r(&t, &normalized) == NULL)
        return (time_t)-1;
    *tm = normalized;
    return t;
}

time_t mktime(struct tm* tm) {
    return timegm(tm);
}
