#include <stdint.h>
#include <stdbool.h>

#include <kernel/util/wallclock.h>
#include <kernel/util/timer.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/fs/fs.h>

// Wall time at timer tick 0 (boot). Valid once wallclock_valid is set.
static uint64_t wallclock_base_ms = 0;
static bool     wallclock_valid   = false;
static bool   (*wallclock_writer)(uint64_t unix_ms) = 0;

void wallclock_set_writer(bool (*write)(uint64_t unix_ms)) {
    wallclock_writer = write;
}

bool wallclock_change(uint64_t unix_ms) {
    if (wallclock_writer == 0 || !wallclock_writer(unix_ms))
        return false;
    wallclock_set(unix_ms);
    return true;
}

void wallclock_set(uint64_t unix_ms) {
    bool first;
    uint32_t flags = irq_save();
    uint64_t now = timer_get_ms();
    wallclock_base_ms = (unix_ms > now) ? unix_ms - now : 0;
    first = !wallclock_valid;
    wallclock_valid = true;
    irq_restore(flags);

    // Files created from now on get real time stamps
    if (first)
        fs_set_time_source(wallclock_now_s);
}

bool wallclock_now_ms(uint64_t* out) {
    uint32_t flags = irq_save();
    bool valid = wallclock_valid;
    uint64_t wall = wallclock_base_ms + timer_get_ms();
    irq_restore(flags);
    if (valid && out != 0)
        *out = wall;
    return valid;
}

uint32_t wallclock_now_s(void) {
    uint64_t ms;
    if (!wallclock_now_ms(&ms))
        return 0;
    uint64_t s = ms / 1000u;
    return (s > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)s;
}

// Days since 1970-01-01 (Howard Hinnant's days_from_civil)
int64_t wallclock_from_civil(int32_t year, uint32_t month, uint32_t day,
                             uint32_t hour, uint32_t minute, uint32_t second) {
    int32_t y = year - (month <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (month > 2 ? month - 3 : month + 9) + 2u) / 5u + day - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;
}

// Howard Hinnant's civil_from_days. Only for times from 1970 on: the
// kernel has unsigned 64-bit division only (div6432.c).
void wallclock_to_civil(int64_t seconds, int32_t* year, uint32_t* month, uint32_t* day,
                        uint32_t* hour, uint32_t* minute, uint32_t* second, uint32_t* weekday) {
    uint64_t s = (seconds > 0) ? (uint64_t)seconds : 0;
    uint32_t days = (uint32_t)(s / 86400u);             // Fits until the year 11 million
    uint32_t rem  = (uint32_t)(s % 86400u);
    *hour    = rem / 3600u;
    *minute  = rem / 60u % 60u;
    *second  = rem % 60u;
    *weekday = (days + 4u) % 7u;                        // 1970-01-01 was a Thursday

    uint32_t z = days + 719468u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *day   = doy - (153 * mp + 2) / 5 + 1;
    *month = (mp < 10) ? mp + 3 : mp - 9;
    *year  = (int32_t)(yoe + era * 400u + (*month <= 2));
}
