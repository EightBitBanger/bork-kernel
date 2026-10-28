// FatFs glue: the disk, clock and character functions FatFs asks its host
// for (fatfs/source/diskio.h, ff.h), on top of the block layer.
//
// FatFs addresses disks by drive number (0..FF_VOLUMES-1, the "N:" at the
// front of its paths). fat_drive_claim binds a free number to a BlockDevice
// for as long as a mount uses it. Every call arrives with the VFS lock held,
// so the drive table and the bounce buffer need no locking of their own.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/fs/fat/fat.h>
#include <kernel/fs/fs.h>
#include <kernel/block/block.h>
#include <kernel/util/string.h>

#include <kernel/fs/fatfs/source/ff.h>
#include <kernel/fs/fatfs/source/diskio.h>

struct FatDrive {
    struct BlockDevice* device;     // NULL: the number is free
    uint32_t sector_limit;          // Sectors f_mkfs may use, 0 = the whole device
};

static struct FatDrive fat_drives[FF_VOLUMES];

// Every transfer goes through here rather than straight into the caller's
// buffer (FatFs hands disk_read file buffers it was given). Static, so DMA
// drivers can reach it.
#define FAT_BOUNCE_SECTORS  8
static uint8_t fat_bounce[FAT_BOUNCE_SECTORS * FF_MAX_SS] __attribute__((aligned(16)));

//
// Drive numbers
//

int fat_drive_claim(struct BlockDevice* device) {
    for (int i = 0; i < FF_VOLUMES; i++) {
        if (fat_drives[i].device == NULL) {
            fat_drives[i].device = device;
            fat_drives[i].sector_limit = 0;
            return i;
        }
    }
    return -1;
}

void fat_drive_release(int drive) {
    if (drive >= 0 && drive < FF_VOLUMES) {
        fat_drives[drive].device = NULL;
        fat_drives[drive].sector_limit = 0;
    }
}

void fat_drive_set_limit(int drive, uint32_t sectors) {
    if (drive >= 0 && drive < FF_VOLUMES)
        fat_drives[drive].sector_limit = sectors;
}

static struct BlockDevice* fat_drive_device(BYTE pdrv) {
    if (pdrv >= FF_VOLUMES)
        return NULL;
    struct BlockDevice* device = fat_drives[pdrv].device;
    if (device == NULL || device->sector_size != FF_MAX_SS)
        return NULL;
    return device;
}

//
// Disk functions
//

DSTATUS disk_status(BYTE pdrv) {
    return (fat_drive_device(pdrv) != NULL) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    struct BlockDevice* device = fat_drive_device(pdrv);
    if (device == NULL)
        return RES_NOTRDY;

    while (count > 0) {
        UINT run = (count < FAT_BOUNCE_SECTORS) ? count : FAT_BOUNCE_SECTORS;
        if (!block_read(device, sector, run, fat_bounce))
            return RES_ERROR;
        memcpy(buff, fat_bounce, (size_t)run * FF_MAX_SS);
        buff   += (size_t)run * FF_MAX_SS;
        sector += run;
        count  -= run;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    struct BlockDevice* device = fat_drive_device(pdrv);
    if (device == NULL)
        return RES_NOTRDY;

    while (count > 0) {
        UINT run = (count < FAT_BOUNCE_SECTORS) ? count : FAT_BOUNCE_SECTORS;
        memcpy(fat_bounce, buff, (size_t)run * FF_MAX_SS);
        if (!block_write(device, sector, run, fat_bounce))
            return RES_ERROR;
        buff   += (size_t)run * FF_MAX_SS;
        sector += run;
        count  -= run;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    struct BlockDevice* device = fat_drive_device(pdrv);
    if (device == NULL)
        return RES_NOTRDY;

    switch (cmd) {
        case CTRL_SYNC:
            return block_flush(device) ? RES_OK : RES_ERROR;

        case GET_SECTOR_COUNT: {
            // FatFs addresses 32-bit sectors (FF_LBA64 off): 2 TB at most
            uint64_t sectors = device->sector_count;
            if (fat_drives[pdrv].sector_limit != 0 && fat_drives[pdrv].sector_limit < sectors)
                sectors = fat_drives[pdrv].sector_limit;
            if (sectors > 0xFFFFFFFFULL)
                sectors = 0xFFFFFFFFULL;
            *(LBA_t*)buff = (LBA_t)sectors;
            return RES_OK;
        }

        case GET_SECTOR_SIZE:
            *(WORD*)buff = (WORD)device->sector_size;
            return RES_OK;

        case GET_BLOCK_SIZE:
            // Erase block unknown: align the data area to 4 KB, which suits
            // flash pages without wasting much of a small volume
            *(DWORD*)buff = 8;
            return RES_OK;
    }
    return RES_PARERR;
}

//
// Clock
//

// FAT timestamps are a local date and time; the kernel clock is UTC
// seconds since 1970, which is what gets stored. Before a clock source is
// set (fs_time_now() == 0) files are stamped 2025-01-01.
DWORD get_fattime(void) {
    return fat_time_from_unix(fs_time_now());
}

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm)
static void fat_civil_from_days(int32_t days, int32_t* year, uint32_t* month, uint32_t* day) {
    days += 719468;
    int32_t era = (days >= 0 ? days : days - 146096) / 146097;
    uint32_t doe = (uint32_t)(days - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *day   = doy - (153 * mp + 2) / 5 + 1;
    *month = (mp < 10) ? mp + 3 : mp - 9;
    *year  = y + (*month <= 2);
}

static int32_t fat_days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

uint32_t fat_time_from_unix(uint32_t seconds) {
    if (seconds == 0)
        return ((uint32_t)(2025 - 1980) << 25) | (1U << 21) | (1U << 16);

    int32_t year;
    uint32_t month, day;
    fat_civil_from_days((int32_t)(seconds / 86400), &year, &month, &day);
    if (year < 1980) return (1U << 21) | (1U << 16);         // 1980-01-01
    if (year > 2107) year = 2107;

    uint32_t rest = seconds % 86400;
    return ((uint32_t)(year - 1980) << 25) | (month << 21) | (day << 16) |
           ((rest / 3600) << 11) | (((rest / 60) % 60) << 5) | ((rest % 60) / 2);
}

uint32_t fat_time_to_unix(uint16_t date, uint16_t time) {
    if (date == 0)
        return 0;
    uint32_t month = (date >> 5) & 0x0F, day = date & 0x1F;
    if (month < 1 || month > 12 || day < 1)
        return 0;
    int32_t days = fat_days_from_civil(1980 + (date >> 9), month, day);
    return (uint32_t)days * 86400U + ((time >> 11) & 0x1F) * 3600U +
           ((time >> 5) & 0x3F) * 60U + (time & 0x1F) * 2U;
}

//
// Characters
//
// FatFs passes names as UTF-8 (FF_LFN_UNICODE 2) and keeps long names in
// UTF-16, which needs no tables. Only 8.3 names are in an OEM code page;
// the kernel knows ASCII there and nothing else, so a non-ASCII character
// in a short name has no conversion (0), and a new file's short alias gets
// '_' in its place, as FatFs does for any character it cannot convert.

WCHAR ff_oem2uni(WCHAR oem, WORD cp) {
    (void)cp;
    return (oem < 0x80) ? oem : 0;
}

WCHAR ff_uni2oem(DWORD uni, WORD cp) {
    (void)cp;
    return (uni < 0x80) ? (WCHAR)uni : 0;
}

// Upper case for name matching: ASCII and Latin-1
DWORD ff_wtoupper(DWORD uni) {
    if (uni >= 'a' && uni <= 'z')
        return uni - 0x20;
    if (uni >= 0xE0 && uni <= 0xFE && uni != 0xF7)
        return uni - 0x20;
    if (uni == 0xFF)
        return 0x178;
    return uni;
}
