#ifndef DWM_THEME_H
#define DWM_THEME_H

/*
 * System-wide color theme.
 *
 * Every color the DWM draws with comes from here: the desktop background,
 * window frames, and the client colors window procedures are expected to
 * use. Read it with dwm_theme(); change it with dwm_theme_set(), which
 * repaints the whole screen.
 *
 * Colors are 0xAARRGGBB. The values in the comments are the defaults (the
 * same as the kernel DWM's).
 */

#include <stdint.h>

typedef struct {
    uint32_t background;            // 0xFF000733
    uint32_t icon_text;             // 0xFFFFFFFF  Icon labels
    uint32_t icon_shadow;           // 0xC0000000  Behind the labels, 1 px down-right
    uint32_t icon_selected;         // 0x5004C004  Selected icon fill (blended)
    uint32_t icon_selected_border;  // 0xFF04C004
    uint32_t band_fill;             // 0x3004C004  Selection rectangle (blended)
    uint32_t band_border;           // 0xD004C004
} dwm_theme_desktop_t;

// Window frame drawn by the DWM (border, titlebar, minimize and close boxes, resize grip)
typedef struct {
    uint32_t border;                // 0xFF2A2A2A
    uint32_t background;            // 0xFF6F6F6F
    uint32_t title_text;            // 0xFFEFEFEF
    uint32_t active_low;            // 0xFF007000  Focused titlebar (top)
    uint32_t active_high;           // 0xFF001900  Focused titlebar (bottom)
    uint32_t inactive_low;          // 0xFF505050  Unfocused titlebar (top)
    uint32_t inactive_high;         // 0xFF101010  Unfocused titlebar (bottom)
} dwm_theme_frame_t;

// Client area of windows: the DWM fills it with `background` before each
// paint, the rest is for window procedures
typedef struct {
    uint32_t background;            // 0xFF141420
    uint32_t background_danger;     // 0xFF100101
    uint32_t text;                  // 0xFFFFFFFF
    uint32_t text_soft;             // 0xFFD0D0DF
    uint32_t text_value;            // 0xFF3FFF3F
    uint32_t text_muted;            // 0xFF8888AA
    uint32_t text_mount;            // 0xFFFDA008
    uint32_t text_danger;           // 0xFFC00404
    uint32_t accent;                // 0xFF04C004
    uint32_t divider_soft;          // 0xFF086008
} dwm_theme_client_t;

typedef struct {
    uint32_t fill;                  // 0xFF1C1C2A
    uint32_t border;                // 0xFF444466
    uint32_t border_active;         // 0xFF04C004
    uint32_t text;                  // 0xFF3FFF3F
    uint32_t fill_pressed;          // 0xFF04C004
    uint32_t border_pressed;        // 0xFF3FFF3F
} dwm_theme_button_t;

typedef struct {
    uint32_t background;            // 0xFF202020
    uint32_t border;                // 0xFF404040
    uint32_t text;                  // 0xFF08F008
    uint32_t cursor;                // 0xFFF0F0F0
    uint32_t selection;             // 0xFF2E5C2E
} dwm_theme_edit_t;

typedef struct {
    uint32_t background;            // 0xE0222222
    uint32_t border;                // 0xE0444444
    uint32_t separator;             // 0xE0111111
    uint32_t highlight;             // 0xE0777777
    uint32_t text;                  // 0xF0E0E0E0
} dwm_theme_menu_t;

typedef struct {
    uint32_t gradient_low;          // 0xFF000000
    uint32_t gradient_high;         // 0xFF00C000
} dwm_theme_taskbar_t;

typedef struct {
    uint32_t pie_disksize_used;     // 0xFFFF0000
    uint32_t pie_disksize_free;     // 0xFF0000FF
} dwm_theme_ui_t;

typedef struct {
    dwm_theme_desktop_t desktop;
    dwm_theme_frame_t   frame;
    dwm_theme_client_t  client;
    dwm_theme_button_t  button;
    dwm_theme_edit_t    edit;
    dwm_theme_menu_t    menu;
    dwm_theme_taskbar_t taskbar;
    dwm_theme_ui_t      interface;
} dwm_theme_t;

// The active theme. The pointer stays valid; its contents change with
// dwm_theme_set().
const dwm_theme_t* dwm_theme(void);

// Copy the built-in default theme into *out
void dwm_theme_default(dwm_theme_t* out);

// Replace the active theme and repaint everything with it. Typical use: copy
// the current theme, change a few fields, set it.
void dwm_theme_set(const dwm_theme_t* theme);

// Shortcut: change only the desktop background color (repaints the screen).
// With a wallpaper (background.h) it shows around and behind it.
void dwm_set_background(uint32_t color);

#endif

