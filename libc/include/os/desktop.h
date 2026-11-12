#ifndef _OS_DESKTOP_H
#define _OS_DESKTOP_H

/*
 * Desktop settings in the registry: what the desktop's background shows.
 *
 * They live in the user's hive (saved in <home>/sys/hku), so any program
 * with file write clearance can change them (os/registry.h os_reg_set).
 * Every value is a string, so they can be read and typed by hand:
 *
 *   hkey_user/control/desktop
 *     background   "color" or "wallpaper"       what the desktop shows
 *     color        "#RRGGBB"                    the background color; also
 *                                               what shows around a
 *                                               wallpaper that doesn't
 *                                               cover the screen
 *     wallpaper    "/mnt/ata0p1/usr/sky.png"    an absolute path
 *     style        "center", "tile", "stretch", how the wallpaper is laid
 *                  "fit" or "fill"              on the screen
 *
 * A value that is missing or can't be read falls back to its default: the
 * theme's color, "wallpaper", <home>/sys/wallpaper/bluefade
 * (OS_DESKTOP_DEFAULT_WALLPAPER, below <home>/sys) and "stretch". So a new
 * system shows the bluefade picture stretched over the screen until the
 * user picks something else. A wallpaper that can't be loaded shows the
 * color (the desktop says why in a notification).
 *
 * After changing them, call wm_settings_changed() (os/wm.h): the desktop
 * reads them again and repaints. The "desktop" program in
 * <home>/sys/settings does all of this.
 *
 * Wallpaper images: PNG, JPEG, BMP, GIF (first frame), TGA, PSD, PIC and
 * PNM (the desktop decodes them with os/image.h). At most 16384 pixels a
 * side and 16 megapixels.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_DESKTOP_KEY          "hkey_user/control/desktop"

#define OS_DESKTOP_BACKGROUND   "background"    // OS_DESKTOP_SHOW_*
#define OS_DESKTOP_COLOR        "color"
#define OS_DESKTOP_WALLPAPER    "wallpaper"
#define OS_DESKTOP_STYLE        "style"         // OS_DESKTOP_STYLE_*

#define OS_DESKTOP_SHOW_COLOR       "color"
#define OS_DESKTOP_SHOW_WALLPAPER   "wallpaper"

#define OS_DESKTOP_STYLE_CENTER     "center"    // Actual size, in the middle
#define OS_DESKTOP_STYLE_TILE       "tile"      // Actual size, repeated from the top-left
#define OS_DESKTOP_STYLE_STRETCH    "stretch"   // Screen-sized, shape not kept
#define OS_DESKTOP_STYLE_FIT        "fit"       // As big as fits whole, color around it
#define OS_DESKTOP_STYLE_FILL       "fill"      // Covers the screen, edges cut off

// Used when the registry has no value (see the top)
#define OS_DESKTOP_DEFAULT_SHOW         OS_DESKTOP_SHOW_WALLPAPER
#define OS_DESKTOP_DEFAULT_WALLPAPER    "wallpaper/bluefade"     // Below <home>/sys (os_syspath)
#define OS_DESKTOP_DEFAULT_STYLE        OS_DESKTOP_STYLE_STRETCH

// "#RRGGBB" with this many bytes, terminator included
#define OS_DESKTOP_COLOR_TEXT_MAX   8

// "#RRGGBB" or "RRGGBB" (either case) -> 0xFFRRGGBB in *out. False, *out
// untouched, for anything else.
static inline bool os_desktop_parse_color(const char* text, uint32_t* out) {
    if (text == NULL || out == NULL)
        return false;
    if (text[0] == '#')
        text++;
    uint32_t color = 0;
    for (int i = 0; i < 6; i++) {
        char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9')      digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else return false;
        color = (color << 4) | digit;
    }
    if (text[6] != '\0')
        return false;
    *out = 0xFF000000u | color;
    return true;
}

// 0xAARRGGBB -> "#RRGGBB" (alpha left out) into out, which holds at least
// OS_DESKTOP_COLOR_TEXT_MAX bytes
static inline void os_desktop_format_color(uint32_t color, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '#';
    for (int i = 0; i < 6; i++)
        out[1 + i] = hex[(color >> (20 - 4 * i)) & 0xF];
    out[7] = '\0';
}

#endif
