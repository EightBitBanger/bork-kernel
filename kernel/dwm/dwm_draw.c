#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/string.h>

#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_display.h>

// ==========================================
// Graphics state
// ==========================================

// The compositor's context on the display back surface (DWM thread only)
static GfxContext screen_gc;
static bool       screen_gc_ready = false;

void dwm_graphics_init(void) {
    gfx_context_init(&screen_gc, gfx_display_back());
    screen_gc_ready = true;
}

GfxContext* dwm_screen_context(void) {
    if (!screen_gc_ready) dwm_graphics_init();
    return &screen_gc;
}

void dwm_window_surface(const struct WindowObject* window, GfxSurface* out) {
    if (window == NULL) {
        gfx_surface_wrap(out, NULL, 0, 0, 0);
        return;
    }
    // Child windows have no buffer of their own: a zero-sized surface, so
    // anything drawn into it is clipped away instead of crashing
    gfx_surface_wrap(out, window->frame_buffer, (int32_t)window->buffer_w, (int32_t)window->buffer_h,
                     (int32_t)window->buffer_w);
}

GfxContext* dwm_window_paint_context(WindowHandle handle) {
    DWM_GUARD();
    if (context.event_window == NULL || context.paint_context == NULL) return NULL;
    if (handle != 0 && context.event_window->id != handle) return NULL;
    return context.paint_context;
}

// Reset per frame: nothing that ran last frame can leave a clip or origin behind
static GfxContext* dwm_frame_context(void) {
    GfxContext* gc = dwm_screen_context();
    gfx_set_target(gc, gfx_display_back());
    gfx_set_clip(gc, NULL);
    gfx_set_origin(gc, 0, 0);
    gfx_set_blend(gc, GFX_BLEND_COPY);
    gfx_set_font(gc, NULL);
    return gc;
}

// ==========================================
// Window tree
// ==========================================

struct WindowObject* dwm_get_root_parent(struct WindowObject* window) {
    struct WindowObject* root = window;
    while (root->parent != NULL) {
        root = root->parent;
    }
    return root;
}

void dwm_sync_child_positions(struct WindowObject* parent) {
    if (parent == NULL) return;

    struct list_node* current = parent->children_head;
    while (current != NULL) {
        struct WindowObject* child = (struct WindowObject*)current->data;

        // Update child's absolute positions using its local offsets and parent surface bounds
        child->x = parent->surface_x + child->local_x;
        child->y = parent->surface_y + child->local_y;

        // Re-calculate the child's client surface bounds based on its new absolute position
        child->surface_x = child->x;
        child->surface_y = child->y + child->titlebar_height + (child->border_width ? 1 : 0);

        // Propagate updates further down the layout tree if this child has children
        dwm_sync_child_positions(child);

        current = current->next;
    }
}

// Let the window procedure repaint its buffer. The paint context is
// published to dwm_window_paint_context() and bound for the legacy draw_*
// calls for exactly as long as the procedure runs.
static void dwm_paint_window(struct WindowObject* window) {
    GfxSurface surface;
    dwm_window_surface(window, &surface);

    GfxContext gc;
    gfx_context_init(&gc, &surface);

    GfxContext* old_bound = draw_bind_context(&gc);
    context.event_window  = window;
    context.paint_context = &gc;

    if (window->event_callback != NULL) {
        window->event_callback(window->id, DWM_EVENT_REDRAW, 0, 0);
    }

    context.paint_context = NULL;
    context.event_window  = NULL;
    draw_bind_context(old_bound);
}

void dwm_render_window_recursive(struct WindowObject* window, const struct WindowContext* ctx) {
    if (window == NULL) return;

    if (window->flags & DWM_WFLAG_REDRAW) {
        window->flags &= ~DWM_WFLAG_REDRAW;
        dwm_paint_window(window);
    }

    GfxRect frame = gfx_rect(window->x - window->border_width, window->y - window->border_width,
                             window->w + (window->border_width * 2), window->h + (window->border_width * 2));
    GfxRect client = gfx_rect(window->x, window->y + window->titlebar_height,
                              window->w, window->h - window->titlebar_height);

    bool dirty_intersection = false;
    bool needs_redecorate = (window->flags & DWM_WFLAG_REDECORATE) != 0;

    for (int i = 0; i < ctx->dirty_count; i++) {
        struct Rect r = ctx->dirty_regions[i];
        GfxRect dirty = gfx_rect(r.x, r.y, r.w, r.h);
        if (!gfx_rect_intersects(&dirty, &frame)) continue;

        dirty_intersection = true;

        // Dirty area reaching past the client area touches the decoration
        if (dirty.x < client.x || dirty.y < client.y ||
            dirty.x + dirty.w > client.x + client.w ||
            dirty.y + dirty.h > client.y + client.h) {
            needs_redecorate = true;
        }
    }

    bool refresh = (window->flags & (DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE)) != 0;

    if (refresh || dirty_intersection) {
        if (window->parent == NULL) {
            bool uploaded_chunk = false;
            for (int i = 0; i < ctx->dirty_count; i++) {
                struct Rect r = ctx->dirty_regions[i];
                GfxRect dirty = gfx_rect(r.x, r.y, r.w, r.h);
                GfxRect clip;
                if (gfx_rect_intersect(&dirty, &client, &clip)) {
                    dwm_upload_window_buffer(window, clip.x, clip.y, clip.w, clip.h);
                    uploaded_chunk = true;
                }
            }

            if (!uploaded_chunk && (window->flags & DWM_WFLAG_REFRESH)) {
                dwm_upload_window_buffer(window, client.x, client.y, client.w, client.h);
            }
        } else {
            // A child has no buffer: its area of the root's buffer is shown
            struct WindowObject* root = dwm_get_root_parent(window);
            int child_abs_x, child_abs_y;
            dwm_get_absolute_position(window, &child_abs_x, &child_abs_y);

            GfxRect area = gfx_rect(child_abs_x, child_abs_y, window->buffer_w, window->buffer_h);
            GfxRect parent_surface = gfx_rect(window->parent->surface_x, window->parent->surface_y,
                                              window->parent->surface_w, window->parent->surface_h);
            GfxRect clip;
            if (gfx_rect_intersect(&area, &parent_surface, &clip)) {
                dwm_upload_window_buffer(root, clip.x, clip.y, clip.w, clip.h);
            }
        }

        if (needs_redecorate) {
            dwm_draw_window(window);
        }

        window->flags &= ~(DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
    }

    struct list_node* current = window->children_head;
    while (current != NULL) {
        struct WindowObject* child = (struct WindowObject*)current->data;
        dwm_render_window_recursive(child, ctx);
        current = current->next;
    }
}

// ==========================================
// Desktop
// ==========================================

static bool dwm_rect_is_dirty(const struct WindowContext* ctx, const GfxRect* rect) {
    for (int i = 0; i < ctx->dirty_count; i++) {
        struct Rect r = ctx->dirty_regions[i];
        GfxRect dirty = gfx_rect(r.x, r.y, r.w, r.h);
        if (gfx_rect_intersects(&dirty, rect)) return true;
    }
    return false;
}

static void dwm_draw_context_menus(GfxContext* gc) {
    for (uint8_t m = 0; m < ctxmenu.menu_count; m++) {
        struct ContextMenu* menu = &ctxmenu.menus[m];
        if (!menu->visible) continue;

        uint16_t menu_height = menu->item_height * menu->item_count;
        menu->h = menu_height;

        gfx_fill_rect(gc, menu->x, menu->y, menu->w, menu_height, menu->color_bg);
        gfx_draw_rect(gc, menu->x, menu->y, menu->w, menu_height, menu->color_border);

        for (unsigned int i = 0; i < menu->item_count; i++) {
            int32_t item_x = menu->x + 1;
            int32_t item_y = menu->y + menu->item_height * i;
            int32_t item_w = menu->w - 3;

            if ((int)i == menu->hovered_item) {
                gfx_fill_rect(gc, item_x, item_y + 1, item_w + 1, menu->item_height - 2, menu->color_highlight);
            }

            if (i > 0 && (int)i != menu->hovered_item && (int)(i - 1) != menu->hovered_item) {
                gfx_draw_hline(gc, item_x, item_y, item_w + 1, menu->color_separator);
            }

            gfx_draw_text(gc, item_x + 3, item_y + 7, menu->item[i].name, menu->color_text);
        }
    }
}

void dwm_draw_desktop(const struct WindowContext* ctx) {
    GfxContext* gc = dwm_frame_context();

    // Clear out background pixels for all localized dirty regions
    for (int i = 0; i < ctx->dirty_count; i++) {
        struct Rect r = ctx->dirty_regions[i];
        gfx_fill_rect(gc, r.x, r.y, r.w, r.h, theme.desktop.background);
    }

    GfxRect bar = gfx_rect(0, display_get_height() - taskbar.height, display_get_width(), taskbar.height);
    if (dwm_rect_is_dirty(ctx, &bar)) {
        gfx_fill_rect(gc, bar.x, bar.y, bar.w, bar.h, theme.desktop.background);
    }

    // Desktop icons

    for (struct list_node* node = workspace.icon_head; node != NULL; node = node->next) {
        struct IconObject* icon = (struct IconObject*)node->data;

        GfxRect bounds = gfx_rect(icon->x + icon->bounds_x, icon->y + icon->bounds_y,
                                  icon->bounds_w, icon->bounds_h);
        if (!dwm_rect_is_dirty(ctx, &bounds)) continue;

        if (icon->icon_sprite != NULL) {
            gfx_set_blend(gc, GFX_BLEND_ALPHA);
            gfx_draw_pixels(gc, icon->icon_sprite->data, icon->width, icon->height,
                            icon->x, icon->y, 0xFF000000, true);
            gfx_set_blend(gc, GFX_BLEND_COPY);
        } else {
            gfx_fill_rect(gc, icon->x, icon->y, icon->width, icon->height, theme.desktop.icon_text);
        }

        // The rename box replaces the label while the icon is renamed
        if (icon != dwm_desktop_rename_icon()) {
            int32_t text_w = gfx_text_width(gfx_get_font(gc), icon->name);
            int32_t text_x = icon->x + ((int32_t)icon->width - text_w) / 2;
            gfx_draw_text(gc, text_x, icon->y + 45, icon->name, theme.desktop.icon_text);
        }
    }

    // Desktop rename box (above the icons, below every window)
    dwm_desktop_rename_draw(ctx);

    // Sync positions before drawing
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        if (window->parent == NULL) {
            dwm_sync_child_positions(window);
        }
    }

    // Normal windows: root windows that are NOT topmost and NOT children

    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        if ((window->style & DWM_WSTYLE_TOPMOST) || (window->parent != NULL) || (window->style & DWM_WSTYLE_CHILD))
            continue;
        dwm_render_window_recursive(window, ctx);
    }

    // Topmost windows

    struct WindowObject* taskbar_window = dwm_get_window_by_id(taskbar.window);

    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        if (!(window->style & DWM_WSTYLE_TOPMOST) || (window->parent != NULL) || (window->style & DWM_WSTYLE_CHILD))
            continue;

        // The taskbar goes on last (below)
        if (window == taskbar_window) continue;

        dwm_render_window_recursive(window, ctx);
    }

    // Taskbar over every other topmost window, whatever the z-order list says
    // (clicking it moves it to the tail). The start menu slides out from
    // behind it this way instead of across it.
    if (taskbar_window != NULL && taskbar_window->parent == NULL) {
        dwm_render_window_recursive(taskbar_window, ctx);
    }

    dwm_draw_context_menus(gc);

    // Cursor
    gfx_set_blend(gc, GFX_BLEND_ALPHA);
    gfx_draw_pixels(gc, images.current_cursor.data, ctx->cursor_width, ctx->cursor_height,
                    ctx->mouse.x, ctx->mouse.y, 0xFF000000, true);
    gfx_set_blend(gc, GFX_BLEND_COPY);
}

// ==========================================
// Window frame (border, titlebar, buttons, edit fields)
// ==========================================

static void dwm_draw_edit_field(GfxContext* gc, const struct WindowObject* window, const struct WindowEditField* field) {
    const GfxFont* font = gfx_get_font(gc);

    int32_t field_x = window->surface_x + field->x;
    int32_t field_y = window->surface_y + field->y;

    // Horizontal: centered, or a small left margin; never left of the box
    int32_t text_w = gfx_text_width(font, field->text);
    int32_t offset_x = field->is_centered ? ((int32_t)field->width - text_w) / 2 : 5;
    if (offset_x < 0) offset_x = 0;

    int32_t offset_y = ((int32_t)field->height - font->glyph_height) / 2;
    if (offset_y < 0) offset_y = 0;

    int32_t text_x = field_x + offset_x;
    int32_t text_y = field_y + offset_y;

    gfx_fill_rect(gc, field_x, field_y, field->width, field->height, theme.edit.background);
    gfx_draw_rect(gc, field_x - 1, field_y - 1, field->width + 2, field->height + 2, theme.edit.border);
    gfx_draw_text(gc, text_x, text_y, field->text, theme.edit.text);

    // Two pixel wide cursor
    int32_t cursor_x = text_x + (int32_t)field->cursor_index * font->advance;
    gfx_fill_rect(gc, cursor_x, text_y, 2, font->glyph_height + 1, theme.edit.cursor);
}

void dwm_draw_window(struct WindowObject* window_handle) {
    if (window_handle->style & DWM_WSTYLE_NOBORDERS)
        return;

    GfxContext* gc = dwm_screen_context();

    int32_t wx = window_handle->x;
    int32_t wy = window_handle->y;
    int32_t ww = window_handle->w;
    int32_t wh = window_handle->h;

    bool focused = (window_handle == dwm_get_focused_window());
    uint32_t title_color = focused ? window_handle->title_color_low : window_handle->inactive_color_low;

    if (window_handle->titlebar_height > 0) {
        gfx_fill_rect(gc, wx, wy, ww, window_handle->titlebar_height, title_color);
        gfx_draw_text(gc, wx + 7, wy + 6, window_handle->title, window_handle->title_text_color);

        // Titlebar divider
        gfx_draw_hline(gc, wx, wy + window_handle->titlebar_height - 1, ww + 1, window_handle->border_color);
    }

    // Outer borders
    for (int32_t b = 1; b <= (int32_t)window_handle->border_width; b++) {
        gfx_draw_rect(gc, wx - b, wy - b, ww + (b * 2), wh + (b * 2), window_handle->border_color);
    }

    // Buttons
    gfx_set_blend(gc, GFX_BLEND_ALPHA);
    for (struct list_node* node = window_handle->buttons_head; node != NULL; node = node->next) {
        struct WindowButton* btn = (struct WindowButton*)node->data;
        if (btn->sprite.data == NULL)
            continue;
        gfx_draw_pixels(gc, btn->sprite.data, btn->sprite.width, btn->sprite.height,
                        wx + btn->x, wy + btn->y, 0xFF000000, true);
    }
    gfx_set_blend(gc, GFX_BLEND_COPY);

    // Editable text fields
    for (struct list_node* node = window_handle->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        if (field->is_active)
            dwm_draw_edit_field(gc, window_handle, field);
    }
}

// ==========================================
// Window procedure drawing (paint context of the window in REDRAW)
// ==========================================
//
// Kept for existing window procedures. They draw into the window's buffer
// through context.paint_context and do nothing outside a REDRAW.

void dwm_draw_line(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    if (context.paint_context == NULL) return;
    gfx_draw_line(context.paint_context, x, y, x + w, y + h, color);
}

void dwm_draw_text(int16_t x, int16_t y, const char* text, uint32_t color) {
    if (context.paint_context == NULL || text == NULL) return;
    gfx_draw_text(context.paint_context, x, y, text, color);
}

void dwm_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    if (context.paint_context == NULL) return;
    gfx_draw_rect(context.paint_context, x, y, w, h, color);
}

void dwm_draw_rect_filled(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    if (context.paint_context == NULL) return;
    gfx_fill_rect(context.paint_context, x, y, w, h, color);
}

// The wrappers take (low, high) and paint high at the start, as they always did
void dwm_draw_rect_filled_gradient_vertical(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color_low, uint32_t color_high) {
    GfxContext* gc = context.paint_context;
    if (gc == NULL) return;
    GfxBlend old = gfx_set_blend(gc, GFX_BLEND_ALPHA);
    gfx_fill_gradient(gc, x, y, w, h, color_high, color_low, GFX_GRADIENT_VERTICAL);
    gfx_set_blend(gc, old);
}

void dwm_draw_rect_filled_gradient_horizontal(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color_low, uint32_t color_high) {
    GfxContext* gc = context.paint_context;
    if (gc == NULL) return;
    GfxBlend old = gfx_set_blend(gc, GFX_BLEND_ALPHA);
    gfx_fill_gradient(gc, x, y, w, h, color_high, color_low, GFX_GRADIENT_HORIZONTAL);
    gfx_set_blend(gc, old);
}

void dwm_draw_sprite(int16_t x, int16_t y, struct Image* image) {
    GfxContext* gc = context.paint_context;
    if (gc == NULL || image == NULL) return;
    GfxBlend old = gfx_set_blend(gc, GFX_BLEND_ALPHA);
    gfx_draw_pixels(gc, image->data, image->width, image->height, x, y, 0x00000000, true);
    gfx_set_blend(gc, old);
}

void dwm_set_cursor(uint32_t* sprite, int16_t width, int16_t height) {
    images.current_cursor.data = sprite;
    images.current_cursor.width = width;
    images.current_cursor.height = height;
}
