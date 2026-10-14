// Legacy draw_* API on top of the gfx layer. See draw.h.

#include <kernel/arch/x86/drivers/display/draw.h>
#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_display.h>

// Kept for code that still reads the boot info through it
struct MultibootInfo* vinfo = NULL;

// Console text colors (draw_set_glyph_scheme). Only read by old code.
uint32_t foreground_color  = 0xFF555555;
uint32_t background_color  = 0xFF555555;
uint32_t transparent_color = 0xFF555555;

static GfxContext  default_context;
static bool        default_ready = false;
static GfxContext* bound = NULL;

// Target of draw_set_buffer() (one at a time, as before)
static GfxSurface  buffer_surface;

// Deprecated setup path (draw_set_info + the two buffer setters)
static uint32_t legacy_front = 0;
static uint32_t legacy_back  = 0;

static GfxContext* ctx(void) {
    if (!default_ready) {
        gfx_context_init(&default_context, gfx_display_back());
        default_ready = true;
    }
    return (bound != NULL) ? bound : &default_context;
}

GfxContext* draw_bind_context(GfxContext* context) {
    GfxContext* old = ctx();
    bound = context;
    return old;
}

GfxContext* draw_get_context(void) {
    return ctx();
}

// Run one call under a fixed blend mode, then put the caller's back
#define WITH_BLEND(mode, call) do {                      \
        GfxBlend _old = gfx_set_blend(ctx(), (mode));    \
        call;                                            \
        gfx_set_blend(ctx(), _old);                      \
    } while (0)

//
// State
//

void draw_set_glyph_scheme(uint32_t foreground, uint32_t background, uint32_t transparent) {
    foreground_color  = foreground;
    background_color  = background;
    transparent_color = transparent;
}

void draw_set_origin(int32_t x, int32_t y) {
    gfx_set_origin(ctx(), x, y);
}

void draw_set_clip_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    // Device coordinates: undo the origin gfx_set_clip will add
    int32_t ox, oy;
    gfx_get_origin(ctx(), &ox, &oy);
    GfxRect r = gfx_rect(x - ox, y - oy, w, h);
    gfx_set_clip(ctx(), &r);
}

void draw_set_buffer_default(void) {
    gfx_set_target(ctx(), gfx_display_back());
}

void draw_set_buffer(uint32_t* target_buffer, uint32_t width, uint32_t height) {
    gfx_surface_wrap(&buffer_surface, target_buffer, (int32_t)width, (int32_t)height, (int32_t)width);
    gfx_set_target(ctx(), &buffer_surface);
}

static void legacy_try_init(void) {
    if (vinfo == NULL || legacy_front == 0 || legacy_back == 0) return;
    gfx_display_init(legacy_front, vinfo->framebuffer_pitch, legacy_back,
                     (int32_t)vinfo->framebuffer_width, (int32_t)vinfo->framebuffer_height);
}

void draw_set_info(uint32_t mbi) {
    vinfo = (struct MultibootInfo*)(uintptr_t)mbi;
    legacy_try_init();
}

void draw_set_frame_front_buffer(uint32_t buffer_ptr) {
    legacy_front = buffer_ptr;
    legacy_try_init();
}

void draw_set_frame_back_buffer(uint32_t buffer_ptr) {
    legacy_back = buffer_ptr;
    legacy_try_init();
}

void draw_flush_region(int x, int y, int width, int height) {
    gfx_display_present_rect(x, y, width, height);
}

void draw_flush_display(void) {
    gfx_display_present_all();
}

uint32_t make_color(float a, float r, float g, float b) {
    return gfx_color_from_float(a, r, g, b);
}

//
// Drawing
//

void draw_pixel(int x, int y, uint32_t color) {
    WITH_BLEND(GFX_BLEND_COPY, gfx_set_pixel(ctx(), x, y, color));
}

void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    WITH_BLEND(GFX_BLEND_COPY, gfx_draw_line(ctx(), x0, y0, x1, y1, color));
}

void draw_rect(int x, int y, int width, int height, uint32_t color) {
    WITH_BLEND(GFX_BLEND_COPY, gfx_draw_rect(ctx(), x, y, width, height, color));
}

void draw_rect_filled(int x, int y, int width, int height, uint32_t color) {
    WITH_BLEND(GFX_BLEND_COPY, gfx_fill_rect(ctx(), x, y, width, height, color));
}

void draw_rect_filled_blend(int x, int y, int width, int height, uint32_t color) {
    WITH_BLEND(GFX_BLEND_ALPHA, gfx_fill_rect(ctx(), x, y, width, height, color));
}

void draw_rect_gradient_vertical_blend(int x, int y, int width, int height, uint32_t color_from, uint32_t color_to) {
    WITH_BLEND(GFX_BLEND_ALPHA, gfx_fill_gradient(ctx(), x, y, width, height, color_from, color_to, GFX_GRADIENT_VERTICAL));
}

void draw_rect_gradient_horizontal_blend(int x, int y, int width, int height, uint32_t color_from, uint32_t color_to) {
    WITH_BLEND(GFX_BLEND_ALPHA, gfx_fill_gradient(ctx(), x, y, width, height, color_from, color_to, GFX_GRADIENT_HORIZONTAL));
}

void draw_circle(int xc, int yc, int r, uint32_t color) {
    WITH_BLEND(GFX_BLEND_COPY, gfx_draw_circle(ctx(), xc, yc, r, color));
}

void draw_text(int16_t x, int16_t y, const char* text, uint32_t color) {
    const GfxFont* old = gfx_set_font(ctx(), NULL);
    WITH_BLEND(GFX_BLEND_COPY, gfx_draw_text(ctx(), x, y, text, color));
    gfx_set_font(ctx(), old);
}

void draw_glyph(const uint8_t* glyph_map, int glyph_index, uint16_t glyph_width, uint16_t glyph_height,
                int x, int y, uint32_t fg_color, uint32_t bg_color, int transparent_bg) {
    if (glyph_map == NULL || glyph_index < 0 || glyph_index > 0xFF) return;

    const GfxFont* rom = gfx_font_default();
    GfxFont font = {
        .glyphs       = glyph_map,
        .first        = 0,
        .count        = (glyph_map == rom->glyphs) ? rom->count : 256,
        .glyph_width  = (uint8_t)glyph_width,
        .glyph_height = (uint8_t)((glyph_height > 8) ? 8 : glyph_height),
        .advance      = (uint8_t)glyph_width,
        .line_height  = (uint8_t)glyph_height,
        .fallback     = 0,
    };

    const GfxFont* old = gfx_set_font(ctx(), &font);
    if (transparent_bg)
        WITH_BLEND(GFX_BLEND_COPY, gfx_draw_char(ctx(), x, y, (uint8_t)glyph_index, fg_color));
    else
        WITH_BLEND(GFX_BLEND_COPY, gfx_draw_char_opaque(ctx(), x, y, (uint8_t)glyph_index, fg_color, bg_color));
    gfx_set_font(ctx(), old);
}

void draw_sprite(const uint32_t* sprite_data, int sprite_w, int sprite_h, int x, int y, uint32_t colorkey) {
    // 0xFFFFFFFF meant "opaque sprite, no key"
    bool keyed = (colorkey != 0xFFFFFFFFu);
    WITH_BLEND(GFX_BLEND_COPY, gfx_draw_pixels(ctx(), sprite_data, sprite_w, sprite_h, x, y, colorkey, keyed));
}

void draw_sprite_blend(const uint32_t* sprite_data, int sprite_w, int sprite_h, int x, int y, uint32_t colorkey) {
    WITH_BLEND(GFX_BLEND_ALPHA, gfx_draw_pixels(ctx(), sprite_data, sprite_w, sprite_h, x, y, colorkey, true));
}

void draw_pie_chart(int center_x, int center_y, int radius, const uint32_t* slices, const uint32_t* colors, size_t count) {
    WITH_BLEND(GFX_BLEND_COPY, gfx_draw_pie(ctx(), center_x, center_y, radius, slices, colors, count));
}

void draw_pie_chart_isometric(int center_x, int center_y, int radius, int depth, const uint32_t* slices, const uint32_t* colors, size_t count) {
    WITH_BLEND(GFX_BLEND_COPY, gfx_draw_pie_isometric(ctx(), center_x, center_y, radius, depth, slices, colors, count));
}
