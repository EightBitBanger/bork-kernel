/*
 * The desktop's background from the registry (wallpaper.h).
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <os/desktop.h>
#include <os/os.h>
#include <os/registry.h>

#include "wallpaper.h"
#include "dwm/dwm.h"

// What was last shown, so an unchanged wallpaper isn't decoded again
// (the settings program may change only the color or the style)
static char    shown_path[PATH_MAX];
static int64_t shown_mtime = -1;
static int64_t shown_size  = -1;

static dwm_wallpaper_style_t parse_style(const char* text) {
    static const struct {
        const char*           name;
        dwm_wallpaper_style_t style;
    } styles[] = {
        { OS_DESKTOP_STYLE_CENTER,  DWM_WALLPAPER_CENTER },
        { OS_DESKTOP_STYLE_TILE,    DWM_WALLPAPER_TILE },
        { OS_DESKTOP_STYLE_STRETCH, DWM_WALLPAPER_STRETCH },
        { OS_DESKTOP_STYLE_FIT,     DWM_WALLPAPER_FIT },
        { OS_DESKTOP_STYLE_FILL,    DWM_WALLPAPER_FILL },
    };
    for (size_t i = 0; i < sizeof(styles) / sizeof(styles[0]); i++) {
        if (strcmp(text, styles[i].name) == 0)
            return styles[i].style;
    }
    return DWM_WALLPAPER_STRETCH;      // OS_DESKTOP_DEFAULT_STYLE
}

int wallpaper_decode(const char* path, image_t* out) {
    // PNG, JPEG, BMP, GIF, ... through os/image.h (stb_image)
    return imagelib_decode_file(path, out);
}

// The file's modification time and size, -1 when they can't be had
static void file_stamp(const char* path, int64_t* mtime, int64_t* size) {
    struct stat st;
    if (stat(path, &st) < 0) {
        *mtime = *size = -1;
        return;
    }
    *mtime = (int64_t)st.st_mtime;
    *size  = (int64_t)st.st_size;
}

static void show_color_only(void) {
    dwm_clear_wallpaper();
    shown_path[0] = '\0';
    shown_mtime = shown_size = -1;
}

void wallpaper_load(void) {
    char text[PATH_MAX];

    // The color first: a wallpaper is laid out on it
    dwm_theme_t defaults;
    dwm_theme_default(&defaults);
    uint32_t color = defaults.desktop.background;
    if (os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_COLOR, text, sizeof(text)) == 0)
        os_desktop_parse_color(text, &color);       // Unparsable: the default
    dwm_set_background(color);

    // Nothing set yet: the default wallpaper, stretched (os/desktop.h)
    char show[16];
    if (os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_BACKGROUND, show, sizeof(show)) < 0)
        strlcpy(show, OS_DESKTOP_DEFAULT_SHOW, sizeof(show));
    if (strcmp(show, OS_DESKTOP_SHOW_WALLPAPER) != 0) {
        show_color_only();
        return;
    }

    char style_text[16] = OS_DESKTOP_DEFAULT_STYLE;
    if (os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_STYLE, style_text, sizeof(style_text)) < 0)
        strlcpy(style_text, OS_DESKTOP_DEFAULT_STYLE, sizeof(style_text));
    dwm_wallpaper_style_t style = parse_style(style_text);

    char path[PATH_MAX];
    bool have_path = os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_WALLPAPER, path, sizeof(path)) == 0 &&
                     path[0] != '\0';
    if (!have_path && os_syspath(OS_DESKTOP_DEFAULT_WALLPAPER, path, sizeof(path)) == NULL) {
        show_color_only();
        return;
    }

    // Same file, unchanged since: only the layout may differ
    int64_t mtime, size;
    file_stamp(path, &mtime, &size);
    if (dwm_has_wallpaper() && mtime >= 0 && mtime == shown_mtime && size == shown_size &&
        strcmp(path, shown_path) == 0) {
        dwm_set_wallpaper_style(style);
        return;
    }

    image_t image;
    if (wallpaper_decode(path, &image) < 0) {
        int err = errno;
        show_color_only();
        const char* why = imagelib_decode_reason();
        if (err == EFBIG)
            dwm_notifyf(DWM_NOTIFY_ERROR, "Wallpaper not shown", "%s: too big (at most %u pixels a side, %u megapixels)",
                        path, (unsigned)IMAGELIB_MAX_SIDE, (unsigned)(IMAGELIB_MAX_PIXELS / (1024u * 1024u)));
        else if (err == ENOEXEC && why[0] != '\0' && strcmp(why, "unknown image type") != 0)
            dwm_notifyf(DWM_NOTIFY_ERROR, "Wallpaper not shown", "%s: damaged picture (%s)", path, why);
        else if (err == ENOEXEC)
            dwm_notifyf(DWM_NOTIFY_ERROR, "Wallpaper not shown", "%s: not a picture the desktop reads", path);
        else
            dwm_notifyf(DWM_NOTIFY_ERROR, "Wallpaper not shown", "%s: %s", path, strerror(err));
        return;
    }
    int rc = dwm_set_wallpaper(image.pixels, image.width, image.height, style);
    int err = errno;
    free(image.pixels);
    if (rc < 0) {
        show_color_only();
        dwm_notifyf(DWM_NOTIFY_ERROR, "Wallpaper not shown", "%s: %s", path, strerror(err));
        return;
    }
    strlcpy(shown_path, path, sizeof(shown_path));
    shown_mtime = mtime;
    shown_size  = size;
}
