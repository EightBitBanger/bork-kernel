#ifndef DWM_BACKGROUND_H
#define DWM_BACKGROUND_H

/*
 * Desktop background: the theme's desktop color (theme.h, dwm_set_background),
 * or a wallpaper image laid out on it.
 *
 *     dwm_set_wallpaper(pixels, w, h, DWM_WALLPAPER_FILL);   // copied
 *     dwm_clear_wallpaper();                                  // the color again
 *
 * The wallpaper is laid out once, into a screen-sized picture kept in this
 * program's memory, whenever it or the color changes; repainting a part of
 * the desktop then copies that part straight onto the screen. The color
 * shows where the wallpaper doesn't reach (center, fit) and through its
 * transparent pixels (alpha blended onto the color).
 *
 * Scaling (stretch, fit, fill) takes the nearest pixel.
 *
 * Where the wallpaper comes from is the desktop program's business (it
 * reads the registry, os/desktop.h); the DWM only shows pixels.
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/gfx.h>

typedef enum {
    DWM_WALLPAPER_CENTER = 0,   // Actual size, in the middle
    DWM_WALLPAPER_TILE,         // Actual size, repeated from the top-left corner
    DWM_WALLPAPER_STRETCH,      // To the screen's size, shape not kept
    DWM_WALLPAPER_FIT,          // As big as fits whole, the color around it
    DWM_WALLPAPER_FILL,         // Covers the screen, shape kept, edges cut off
} dwm_wallpaper_style_t;

// Largest wallpaper side in pixels (keeps the 16.16 scaling in 32 bits)
#define DWM_WALLPAPER_MAX_SIDE  16384

// Show `pixels` (width x height, 0xAARRGGBB, stride = width) as the desktop
// background. They are copied: the caller may free them afterwards. 0, or
// -1 with errno (EINVAL, ENOMEM: the color stays / the old wallpaper stays).
int  dwm_set_wallpaper(const uint32_t* pixels, int32_t width, int32_t height,
                       dwm_wallpaper_style_t style);

// Lay the current wallpaper out another way (repaints). No wallpaper: only
// remembered for the next one.
void dwm_set_wallpaper_style(dwm_wallpaper_style_t style);

// Back to the plain color (repaints)
void dwm_clear_wallpaper(void);

bool dwm_has_wallpaper(void);

// Internal (compositor.c, theme.c, icons.c, dwm.c)

// Fill `area` (screen coordinates, already the screen's clip) with the
// background. The screen's origin is (0, 0); its blend mode is left COPY.
void background_paint(gfx_surface_t* screen, gfx_rect_t area);

// The theme's desktop color changed: lay the wallpaper out again on it
void background_color_changed(void);

// Free the wallpaper (dwm_shutdown)
void background_shutdown(void);

#endif
