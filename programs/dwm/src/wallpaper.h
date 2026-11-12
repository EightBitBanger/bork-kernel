#ifndef DESKTOP_WALLPAPER_H
#define DESKTOP_WALLPAPER_H

/*
 * The desktop's background from the registry (os/desktop.h): a color, or a
 * wallpaper image on it.
 *
 *     hkey_user/control/desktop
 *       background   "color" / "wallpaper"
 *       color        "#RRGGBB"
 *       wallpaper    an absolute path
 *       style        "center" / "tile" / "stretch" / "fit" / "fill"
 *
 * wallpaper_load() reads them and hands them to the DWM (dwm/background.h,
 * dwm_set_background). Missing or unreadable values take their defaults
 * (the theme's color, "color", "fill"). A wallpaper that can't be loaded
 * leaves the color showing and says why in a notification.
 *
 * It runs at start-up and again on DWM_EVENT_SETTINGS_CHANGED, which the
 * settings program asks for (wm_settings_changed) after writing the
 * registry.
 *
 * Images: wallpaper_decode() is where a file becomes pixels, through the
 * image library (resources/imagelib.h): PNG, JPEG, BMP, GIF, TGA, PSD, PIC
 * and PNM, decoded with libc's os/image.h (stb_image).
 */

#include "resources/imagelib.h"

void wallpaper_load(void);

// Decode the image file at `path` into *out (the caller frees out->pixels).
// 0, or -1 with errno (ENOEXEC: not a format the desktop reads).
int  wallpaper_decode(const char* path, image_t* out);

#endif
