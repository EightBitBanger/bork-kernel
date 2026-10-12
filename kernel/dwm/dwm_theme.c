#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/console/display.h>
#include <kernel/util/list.h>

// Active theme (internal DWM code reads this directly through `theme`)
struct DWMTheme theme;

static const struct DWMTheme dwm_default_theme = {
    .desktop = {
        .background         = 0xFF000733,
        .icon_text          = 0xFFFFFFFF,
    },
    .frame = {
        .border             = 0xFF2A2A2A,
        .background         = 0xFF6F6F6F,
        .title_text         = 0xFFEFEFEF,
        .active_low         = 0xFF007000,
        .active_high        = 0xFF001900,
        .inactive_low       = 0xFF505050,
        .inactive_high      = 0xFF101010,
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
        .background         = 0x8F222222,
        .border             = 0x8F444444,
        .separator          = 0x8F111111,
        .highlight          = 0x8F777777,
        .text               = 0x8FE0E0E0,
    },
    .taskbar = {
        .gradient_low       = 0xFF000000,
        .gradient_high      = 0xFF00C000,
    },
    .start_menu = {
        .header_low         = 0xFF000000,
        .header_high        = 0xFF006000,
        .row                = 0xFF10101A,
        .row_border         = 0xFF1C1C2A,
        .separator          = 0xFF1C3A1C,
    },
    .chart = {
        .used               = 0xFFF700F7,
        .available          = 0xFF0000F6,
    },
};

void dwm_theme_init(void) {
    theme = dwm_default_theme;
}

void dwm_theme_get_default(struct DWMTheme* out) {
    DWM_GUARD();
    if (out != NULL) *out = dwm_default_theme;
}

const struct DWMTheme* dwm_get_theme(void) {
    return &theme;
}

// Window objects keep their own copy of the frame colors (so a program can
// still recolor one window), so a theme change has to push them out again
static void theme_apply_to_window(struct WindowObject* window) {
    window->border_color        = theme.frame.border;
    window->background_color    = theme.frame.background;
    window->title_text_color    = theme.frame.title_text;
    window->title_color_low     = theme.frame.active_low;
    window->title_color_high    = theme.frame.active_high;
    window->inactive_color_low  = theme.frame.inactive_low;
    window->inactive_color_high = theme.frame.inactive_high;
    
    // REDRAW makes the compositor call the window's REDRAW handler, which
    // picks up the new client colors from dwm_get_theme()
    window->flags |= (DWM_WFLAG_REDRAW | DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
}

void dwm_set_theme(const struct DWMTheme* new_theme) {
    if (new_theme == NULL) return;
    
    DWM_GUARD();
    
    theme = *new_theme;
    
    // Every window (children included) is on the workspace list
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        theme_apply_to_window((struct WindowObject*)node->data);
    }
    
    for (int i = 0; i < MAX_CONTEXT_MENUS; i++) {
        ctxmenu.menus[i].color_bg        = theme.menu.background;
        ctxmenu.menus[i].color_border    = theme.menu.border;
        ctxmenu.menus[i].color_separator = theme.menu.separator;
        ctxmenu.menus[i].color_highlight = theme.menu.highlight;
        ctxmenu.menus[i].color_text      = theme.menu.text;
    }
    
    // Desktop background and icon labels
    dwm_invalidate_region(0, 0, display_get_width(), display_get_height());
}
