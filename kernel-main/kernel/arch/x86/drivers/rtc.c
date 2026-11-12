#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/drivers/rtc.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/util/timer.h>
#include <kernel/util/wallclock.h>

//
// CMOS real-time clock (rtc.h)
//

#define CMOS_INDEX          0x70
#define CMOS_DATA           0x71
#define CMOS_NMI_OFF        0x80

#define RTC_SECONDS         0x00
#define RTC_MINUTES         0x02
#define RTC_HOURS           0x04
#define RTC_WEEKDAY         0x06    // 1-7, Sunday = 1
#define RTC_DAY             0x07
#define RTC_MONTH           0x08
#define RTC_YEAR            0x09
#define RTC_REG_A           0x0A
#define RTC_REG_B           0x0B
#define RTC_REG_D           0x0D
#define RTC_CENTURY         0x32    // Where the ACPI FADT century field points on nearly every PC

#define RTC_A_UIP           0x80    // Update in progress: the time registers are changing
#define RTC_B_SET           0x80    // Updates stopped while the time is written
#define RTC_B_24HOUR        0x02    // Hours 0-23 (clear: 1-12 with bit 7 for PM)
#define RTC_B_BINARY        0x04    // Binary (clear: BCD)
#define RTC_HOUR_PM         0x80

#define RTC_READ_TRIES      8       // Readings to get two that agree
#define RTC_UIP_SPINS       100000  // An update takes under 2 ms
#define RTC_RESYNC_MS       (10u * 60u * 1000u)
#define RTC_EDGE_WAIT_MS    3000    // Give up waiting for the next second after this

static uint8_t cmos_get(uint8_t reg) {
    outb(CMOS_INDEX, reg | CMOS_NMI_OFF);
    return inb(CMOS_DATA);
}

// Point the index at a harmless register with NMIs unmasked, as the
// watchdog leaves it
static void cmos_done(void) {
    outb(CMOS_INDEX, RTC_REG_D);
    (void)inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v >> 4) * 10u + (v & 0x0Fu));
}

typedef struct {
    uint8_t sec, min, hour, day, mon, year, century, b;
} RtcRaw;

// The time registers once, between updates. False if the chip stays
// mid-update (or there is no chip: UIP reads as 1 on a floating bus).
static bool rtc_read_raw(RtcRaw* raw) {
    uint32_t spins = 0;
    while (cmos_get(RTC_REG_A) & RTC_A_UIP) {
        if (++spins > RTC_UIP_SPINS)
            return false;
    }
    raw->sec     = cmos_get(RTC_SECONDS);
    raw->min     = cmos_get(RTC_MINUTES);
    raw->hour    = cmos_get(RTC_HOURS);
    raw->day     = cmos_get(RTC_DAY);
    raw->mon     = cmos_get(RTC_MONTH);
    raw->year    = cmos_get(RTC_YEAR);
    raw->century = cmos_get(RTC_CENTURY);
    raw->b       = cmos_get(RTC_REG_B);
    return true;
}

static bool raw_equal(const RtcRaw* a, const RtcRaw* b) {
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour && a->day == b->day &&
           a->mon == b->mon && a->year == b->year && a->century == b->century && a->b == b->b;
}

static uint8_t days_in_month(int32_t year, uint8_t month) {
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0))
        return 29;
    return days[month - 1];
}

static bool rtc_decode(const RtcRaw* raw, RtcTime* out) {
    bool binary = (raw->b & RTC_B_BINARY) != 0;
    bool pm = !(raw->b & RTC_B_24HOUR) && (raw->hour & RTC_HOUR_PM);
    uint8_t hour = (uint8_t)(raw->hour & ~RTC_HOUR_PM);

    uint8_t sec  = binary ? raw->sec  : bcd_to_bin(raw->sec);
    uint8_t min  = binary ? raw->min  : bcd_to_bin(raw->min);
    uint8_t day  = binary ? raw->day  : bcd_to_bin(raw->day);
    uint8_t mon  = binary ? raw->mon  : bcd_to_bin(raw->mon);
    uint8_t yy   = binary ? raw->year : bcd_to_bin(raw->year);
    uint8_t cent = binary ? raw->century : bcd_to_bin(raw->century);
    hour = binary ? hour : bcd_to_bin(hour);

    if (!(raw->b & RTC_B_24HOUR)) {
        // 12 AM is 0, 12 PM is 12
        if (hour == 12)
            hour = 0;
        if (pm)
            hour = (uint8_t)(hour + 12);
    }

    // A century register that holds nonsense (some boards keep something
    // else at 0x32) means "20xx"
    int32_t year = (cent >= 19 && cent <= 30) ? cent * 100 + yy : 2000 + yy;

    if (sec > 59 || min > 59 || hour > 23 || yy > 99 || mon < 1 || mon > 12 || day < 1 ||
        day > days_in_month(year, mon))
        return false;

    out->year   = year;
    out->month  = mon;
    out->day    = day;
    out->hour   = hour;
    out->minute = min;
    out->second = sec;
    return true;
}

bool rtc_read(RtcTime* out) {
    RtcRaw last, next;
    bool have_last = false;
    bool ok = false;

    uint32_t flags = irq_save();
    for (int i = 0; i < RTC_READ_TRIES && !ok; i++) {
        if (!rtc_read_raw(&next))
            break;
        if (have_last && raw_equal(&last, &next))
            ok = true;
        last = next;
        have_last = true;
    }
    cmos_done();
    irq_restore(flags);

    return ok && rtc_decode(&last, out);
}

static void cmos_put(uint8_t reg, uint8_t value) {
    outb(CMOS_INDEX, reg | CMOS_NMI_OFF);
    outb(CMOS_DATA, value);
}

static uint8_t bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

// Seconds apart, ignoring the date: a reading taken just after a write may
// be a second or two on
static bool rtc_close_to(const RtcTime* a, const RtcTime* b) {
    if (a->year != b->year || a->month != b->month || a->day != b->day)
        return false;
    int32_t sa = a->hour * 3600 + a->minute * 60 + a->second;
    int32_t sb = b->hour * 3600 + b->minute * 60 + b->second;
    return sb - sa >= 0 && sb - sa <= 2;
}

static void rtc_write_once(const RtcTime* t, uint8_t weekday) {
    uint32_t flags = irq_save();
    uint8_t b = (uint8_t)(cmos_get(RTC_REG_B) & ~RTC_B_SET);
    bool binary = (b & RTC_B_BINARY) != 0;
#define ENC(v) (binary ? (uint8_t)(v) : bin_to_bcd((uint8_t)(v)))

    uint8_t hour = t->hour;
    uint8_t pm = 0;
    if (!(b & RTC_B_24HOUR)) {
        pm = (hour >= 12) ? RTC_HOUR_PM : 0;
        hour = (uint8_t)(hour % 12);
        if (hour == 0)
            hour = 12;
    }

    // The century byte is only written where the chip already keeps one:
    // on other boards 0x32 is someone else's
    uint8_t cent_raw = cmos_get(RTC_CENTURY);
    uint8_t cent_now = binary ? cent_raw : bcd_to_bin(cent_raw);
    bool has_century = cent_now >= 19 && cent_now <= 30;

    cmos_put(RTC_REG_B, (uint8_t)(b | RTC_B_SET));     // Stop the updates
    cmos_put(RTC_SECONDS, ENC(t->second));
    cmos_put(RTC_MINUTES, ENC(t->minute));
    cmos_put(RTC_HOURS,   (uint8_t)(ENC(hour) | pm));
    cmos_put(RTC_WEEKDAY, ENC(weekday + 1u));
    cmos_put(RTC_DAY,     ENC(t->day));
    cmos_put(RTC_MONTH,   ENC(t->month));
    cmos_put(RTC_YEAR,    ENC(t->year % 100));
    if (has_century)
        cmos_put(RTC_CENTURY, ENC(t->year / 100));
    cmos_put(RTC_REG_B, b);                             // And run again
#undef ENC
    cmos_done();
    irq_restore(flags);
}

bool rtc_write(const RtcTime* t, uint8_t weekday) {
    if (t->year < 2000 || t->year > 2099 || t->month < 1 || t->month > 12 || t->day < 1 ||
        t->day > days_in_month(t->year, t->month) || t->hour > 23 || t->minute > 59 ||
        t->second > 59 || weekday > 6)
        return false;

    // Read it back: a watchdog NMI between an index and its data byte
    // sends the byte to register C, where it is lost
    for (int attempt = 0; attempt < 3; attempt++) {
        rtc_write_once(t, weekday);
        RtcTime back;
        if (rtc_read(&back) && rtc_close_to(t, &back))
            return true;
    }
    return false;
}

static uint64_t rtc_unix_ms(const RtcTime* t) {
    int64_t s = wallclock_from_civil(t->year, t->month, t->day, t->hour, t->minute, t->second);
    return (s > 0) ? (uint64_t)s * 1000u : 0;
}

//
// The driver
//
// Probe: the first reading, set right away so the clock is there from the
// start (up to a second behind, as the chip only has whole seconds). Poll:
// watch for the chip's seconds to tick over and set the clock exactly then,
// once now and again every RTC_RESYNC_MS.
//

#define RTC_POLL_MS  100u

typedef struct {
    bool     bound;
    bool     waiting_edge;      // Looking for the next second to begin
    uint8_t  edge_second;       // The second seen when we started looking
    uint64_t edge_started;      // timer_get_ms() then
    uint64_t next_sync;         // timer_get_ms() of the next resync
} RtcState;

static RtcState rtc_state;

static void rtc_start_edge_wait(const RtcTime* now) {
    rtc_state.waiting_edge = true;
    rtc_state.edge_second  = now->second;
    rtc_state.edge_started = timer_get_ms();
}

static void rtc_poll(void) {
    if (!rtc_state.bound)
        return;

    uint64_t now = timer_get_ms();
    if (!rtc_state.waiting_edge && now < rtc_state.next_sync)
        return;

    RtcTime t;
    if (!rtc_read(&t))
        return;

    if (!rtc_state.waiting_edge) {
        rtc_start_edge_wait(&t);
        return;
    }

    if (t.second != rtc_state.edge_second) {
        // The second began since the last poll: count from the middle of
        // that gap, which puts the clock within ~50 ms of the chip
        wallclock_set(rtc_unix_ms(&t) + RTC_POLL_MS / 2u);
        rtc_state.waiting_edge = false;
        rtc_state.next_sync = now + RTC_RESYNC_MS;
    } else if (now - rtc_state.edge_started > RTC_EDGE_WAIT_MS) {
        // A stopped clock: keep the coarse time, try again later
        rtc_state.waiting_edge = false;
        rtc_state.next_sync = now + RTC_RESYNC_MS;
    }
}

// The wall clock's writer (wallclock_change, SYS_CLOCK_SET): the whole
// second goes to the chip, and the edge wait starts over so the poll lines
// the running clock up with the chip's next tick again
static bool rtc_set_time(uint64_t unix_ms) {
    if (!rtc_state.bound)
        return false;
    int32_t year;
    uint32_t month, day, hour, minute, second, weekday;
    wallclock_to_civil((int64_t)(unix_ms / 1000u), &year, &month, &day, &hour, &minute, &second, &weekday);
    RtcTime t = {
        .year = year, .month = (uint8_t)month, .day = (uint8_t)day,
        .hour = (uint8_t)hour, .minute = (uint8_t)minute, .second = (uint8_t)second,
    };
    if (!rtc_write(&t, (uint8_t)weekday))
        return false;
    rtc_start_edge_wait(&t);
    return true;
}

static int rtc_probe(struct Device* dev) {
    RtcTime t;
    if (!rtc_read(&t))
        return PROBE_FAILED;

    // Halfway through the current second is the best guess for now
    wallclock_set(rtc_unix_ms(&t) + 500u);

    rtc_state.bound = true;
    rtc_start_edge_wait(&t);
    wallclock_set_writer(rtc_set_time);
    dev->driver_data = &rtc_state;
    return PROBE_BOUND;
}

static void rtc_remove(struct Device* dev) {
    (void)dev;
    rtc_state.bound = false;    // The wall clock keeps running on the PIT
}

static const struct DeviceMatch rtc_match[] = {
    { .bus = BUS_PLATFORM, .name = "rtc" },
    { 0 }
};

static const struct Driver rtc_driver = {
    .name    = "rtc",
    .abi     = DRIVER_ABI,
    .flags   = DRIVER_BUILTIN,
    .version = "1.0",
    .match   = rtc_match,
    .probe   = rtc_probe,
    .remove  = rtc_remove,
    .poll    = rtc_poll,
    .poll_ms = RTC_POLL_MS,
};
DRIVER_BUILTIN_DECLARE(rtc_driver);
