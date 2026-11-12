#include "theme.h"
#include "compositor.h"
#include "background.h"

static const dwm_theme_t default_theme = {
    .desktop = {
        .background             = 0xFF020730,
        .icon_text              = 0xFFFFFFFF,
        .icon_shadow            = 0xC0000000,
        .icon_selected          = 0x5004C004,
        .icon_selected_border   = 0xFF04C004,
        .band_fill              = 0x3004C004,
        .band_border            = 0xD004C004,
    },
    .frame = {
        .border             = 0xFF2A2A2A,
        .background         = 0xFF6F6F6F,
        .title_text         = 0xFFEFEFEF,
        .active_low         = 0xFF007000,
        .active_high        = 0xFF002200,
        .inactive_low       = 0xFF505050,
        .inactive_high      = 0xFF202020,
    },
    .client = {
        .background         = 0xFF141420,
        .background_danger  = 0xFF100101,
        .text               = 0xFFFFFFFF,
        .text_soft          = 0xFFD0D0DF,
        .text_value         = 0xFF3FFF3F,
        .text_muted         = 0xFF8888AA,
        .text_mount         = 0xFFFDA008,
        .text_danger        = 0xFFC00404,
        .accent             = 0xFF04C004,
        .divider_soft       = 0xFF086008,
    },
    .button = {
        .fill               = 0xFF1C1C2A,
        .border             = 0xFF444466,
        .border_active      = 0xFF04C004,
        .text               = 0xFF3FFF3F,
        .fill_pressed       = 0xFF04C004,
        .border_pressed     = 0xFF3FFF3F,
    },
    .edit = {
        .background         = 0xFF202020,
        .border             = 0xFF404040,
        .text               = 0xFF08F008,
        .cursor             = 0xFFF0F0F0,
        .selection          = 0xFF2E5C2E,
    },
    .menu = {
        .background         = 0xE0222222,
        .border             = 0xE0444444,
        .separator          = 0xE0111111,
        .highlight          = 0xE0777777,
        .text               = 0xF0E0E0E0,
    },
    .taskbar = {
        .gradient_low       = 0xFF000000,
        .gradient_high      = 0xFF00C000,
    },
    .interface = {
        .pie_disksize_used  = 0xFFF700F7,
        .pie_disksize_free  = 0xFF0000F6,
    },
};

static dwm_theme_t active = default_theme;

const dwm_theme_t* dwm_theme(void) {
    return &active;
}

void dwm_theme_default(dwm_theme_t* out) {
    if (out != 0)
        *out = default_theme;
}

void dwm_theme_set(const dwm_theme_t* theme) {
    if (theme == 0)
        return;
    bool color_changed = (active.desktop.background != theme->desktop.background);
    active = *theme;
    if (color_changed)
        background_color_changed();
    compositor_invalidate_all();
}

void dwm_set_background(uint32_t color) {
    if (active.desktop.background == color)
        return;
    active.desktop.background = color;
    background_color_changed();     // A wallpaper is laid out on it
    compositor_invalidate_all();
}

