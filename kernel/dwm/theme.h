#ifndef _DWM_THEME_H_
#define _DWM_THEME_H_

// System-wide color theme.
//
// Every color the DWM, its built-in windows (dialogs, taskbar, start menu) and
// the bundled programs (explorer, notepad) draw with comes from here. Read it
// with dwm_get_theme(); change it with dwm_set_theme(), which repaints the
// whole screen with the new colors.
//
// Colors are 0xAARRGGBB. The values in the comments are the defaults.

#include <stdint.h>

// Desktop background and icon labels
struct DWMThemeDesktop {
    uint32_t background;            // 0xFF0E0E1A
    uint32_t icon_text;             // 0xFFFFFFFF
};

// Window frame drawn by the DWM (border + titlebar)
struct DWMThemeFrame {
    uint32_t border;                // 0xFF2A2A2A
    uint32_t background;            // 0xFF6F6F6F
    uint32_t title_text;            // 0xFFEFEFEF
    uint32_t active_low;            // 0xFF008000  Focused titlebar
    uint32_t active_high;           // 0xFF001900
    uint32_t inactive_low;          // 0xFF505050  Unfocused titlebar
    uint32_t inactive_high;         // 0xFF101010
};

// Client area of windows and dialogs
struct DWMThemeClient {
    uint32_t background;            // 0xFF08080F
    uint32_t background_danger;     // 0xFF100101  Destructive dialogs (delete)
    uint32_t text;                  // 0xFFFFFFFF  Labels, messages
    uint32_t text_soft;             // 0xFFD0D0DF  Body text (explorer items, notepad)
    uint32_t text_value;            // 0xFF3FFF3F  Values, highlighted text
    uint32_t text_muted;            // 0xFF8888AA  Inactive tabs, subtitles
    uint32_t text_mount;            // 0xFFFDA008  Mounted part of a path
    uint32_t text_danger;           // 0xFFC00404  Paths about to be deleted
    uint32_t accent;                // 0xFF04C004  Dividers, active tab, checkboxes
    uint32_t divider_soft;          // 0xFF086008  Subtle dividers (explorer nav bar)
};

// Push buttons, tabs and menu bars
struct DWMThemeButton {
    uint32_t fill;                  // 0xFF1C1C2A
    uint32_t border;                // 0xFF444466
    uint32_t border_active;         // 0xFF04C004  Selected tab
    uint32_t text;                  // 0xFF3FFF3F
    uint32_t fill_pressed;          // 0xFF04C004  Toggled (start button while open)
    uint32_t border_pressed;        // 0xFF3FFF3F
};

// Text input: window edit fields, explorer path bar, desktop rename box
struct DWMThemeEdit {
    uint32_t background;            // 0xFF202020
    uint32_t border;                // 0xFF404040
    uint32_t text;                  // 0xFF08F008
    uint32_t cursor;                // 0xFFF0F0F0
    uint32_t selection;             // 0xFF2E5C2E  Selected text background
};

// Right-click context menus
struct DWMThemeMenu {
    uint32_t background;            // 0x8F222222
    uint32_t border;                // 0x8F444444
    uint32_t separator;             // 0x8F111111
    uint32_t highlight;             // 0x8F777777
    uint32_t text;                  // 0x8FE0E0E0
};

struct DWMThemeTaskbar {
    uint32_t gradient_low;          // 0xFF000000
    uint32_t gradient_high;         // 0xFF00C000
};

struct DWMThemeStartMenu {
    uint32_t header_low;            // 0xFF000000
    uint32_t header_high;           // 0xFF006000
    uint32_t row;                   // 0xFF10101A
    uint32_t row_border;            // 0xFF1C1C2A
    uint32_t separator;             // 0xFF1C3A1C
};

// Charts (storage usage pie in Properties)
struct DWMThemeChart {
    uint32_t used;                  // 0xFFF700F7
    uint32_t available;             // 0xFF0000F6
};

struct DWMTheme {
    struct DWMThemeDesktop   desktop;
    struct DWMThemeFrame     frame;
    struct DWMThemeClient    client;
    struct DWMThemeButton    button;
    struct DWMThemeEdit      edit;
    struct DWMThemeMenu      menu;
    struct DWMThemeTaskbar   taskbar;
    struct DWMThemeStartMenu start_menu;
    struct DWMThemeChart     chart;
};

// The active theme. The pointer stays valid for the lifetime of the DWM;
// its contents change when dwm_set_theme is called.
const struct DWMTheme* dwm_get_theme(void);

// Copy the built-in default theme into `out`
void dwm_theme_get_default(struct DWMTheme* out);

// Replace the active theme and repaint everything with it.
// Typical use: get the default (or current) theme, change a few fields, set it.
void dwm_set_theme(const struct DWMTheme* new_theme);

#endif
