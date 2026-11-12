#ifndef _KERNEL_X86_RTC_H_
#define _KERNEL_X86_RTC_H_

// CMOS real-time clock (MC146818 and the chipsets that copy it)
//
// The battery-backed clock every PC has, read through ports 0x70 / 0x71.
// The built-in driver "rtc" (rtc.c) binds the platform device "rtc" that
// driver_init adds, reads the chip once and hands the time to the wall
// clock (util/wallclock.h). After that its poll keeps the wall clock on
// the chip's seconds: the PIT tick it is carried by runs about 13 s a day
// fast (1193182 / 1193 Hz), so every RTC_RESYNC_MS it waits for the chip's
// next second to begin and sets the wall clock to it again.
//
// The chip is written only by rtc_write, which SYS_CLOCK_SET reaches
// through wallclock_change (admin only).
//
// Port 0x70 is shared with the watchdog's NMI handler (scheduler/
// watchdog.c), which reads register C on every RTC periodic interrupt and
// can arrive between our index write and data read even with interrupts
// off. A reading is only taken when two in a row agree, which a stray NMI
// cannot fake.

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t  year;      // 2026
    uint8_t  month;     // 1-12
    uint8_t  day;       // 1-31
    uint8_t  hour;      // 0-23
    uint8_t  minute;    // 0-59
    uint8_t  second;    // 0-59
} RtcTime;

// One consistent reading of the chip, in binary and 24-hour form. False
// when the chip doesn't answer sensibly (no RTC, or values out of range).
bool rtc_read(RtcTime* out);

// Set the chip to t (weekday 0-6, Sunday = 0), in whatever BCD / binary and
// 12 / 24-hour form it keeps. Years 2000-2099 only (the chip holds two
// digits; the century byte is written only where the board has one). True
// once a reading back agrees.
bool rtc_write(const RtcTime* t, uint8_t weekday);

#endif
