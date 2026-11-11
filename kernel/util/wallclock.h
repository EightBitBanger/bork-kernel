#ifndef _KERNEL_UTIL_WALLCLOCK_H_
#define _KERNEL_UTIL_WALLCLOCK_H_

// Wall clock: the date and time of day
//
// The kernel counts time two ways. timer_get_ms() is milliseconds since
// boot from the 1 kHz PIT tick: monotonic, never set. The wall clock is
// milliseconds since 1970-01-01 00:00:00 as the machine's clock chip (on
// x86 the CMOS RTC, drivers/rtc.c) reads it, carried forward between
// readings by the PIT tick:
//
//     wall = base + timer_get_ms()
//
// A clock driver calls wallclock_set() whenever it has a fresh reading;
// everything else just reads. Until a driver has set it there is no wall
// clock (wallclock_now_ms() is false) and callers fall back to uptime.
//
// The chip's time is taken as it is: there are no time zones in the
// kernel. A PC whose firmware clock keeps local time (as Windows sets it)
// shows local time; one that keeps UTC shows UTC.
//
// Safe from any thread and with interrupts off (one irq_save around a
// 64-bit read). Not from the NMI handler.

#include <stdint.h>
#include <stdbool.h>

// The wall clock reads `unix_ms` right now. Sets the clock source (and so
// file time stamps, fs_set_time_source) the first time.
void wallclock_set(uint64_t unix_ms);

// The clock driver's way to write a new time into the chip (rtc.c). True
// when the chip took it. Set once, by the driver that set the clock.
void wallclock_set_writer(bool (*write)(uint64_t unix_ms));

// Change the time (SYS_CLOCK_SET): the chip first, then the running clock.
// False when there is no clock chip or it would not take the time.
bool wallclock_change(uint64_t unix_ms);

// Milliseconds since 1970-01-01 00:00:00 into *out. False (out untouched)
// while no clock driver has set it.
bool wallclock_now_ms(uint64_t* out);

// Whole seconds since 1970, 0 while there is no clock (the shape
// fs_set_time_source wants)
uint32_t wallclock_now_s(void);

// Calendar <-> seconds since 1970 (proleptic Gregorian, no leap seconds).
// wallclock_to_civil also gives the day of the week (0 = Sunday).
int64_t wallclock_from_civil(int32_t year, uint32_t month, uint32_t day,
                             uint32_t hour, uint32_t minute, uint32_t second);
void wallclock_to_civil(int64_t seconds, int32_t* year, uint32_t* month, uint32_t* day,
                        uint32_t* hour, uint32_t* minute, uint32_t* second, uint32_t* weekday);

#endif
