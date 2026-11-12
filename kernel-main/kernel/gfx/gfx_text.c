// Bitmap fonts and text

#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_internal.h>

#include <kernel/util/string.h>

// The char ROM is defined in this header (not just declared), so it is
// included here and nowhere else. Everyone else gets it through
// gfx_font_default().
#include <kernel/arch/x86/drivers/display/char_rom.h>

#define CHAR_ROM_GLYPH_WIDTH   6

static const GfxFont gfx_default_font = {
    .glyphs       = char_rom,
    .first        = 0,
    .count        = sizeof(char_rom) / CHAR_ROM_GLYPH_WIDTH,
    .glyph_width  = CHAR_ROM_GLYPH_WIDTH,
    .glyph_height = 8,
    .advance      = 6,
    .line_height  = 8,
    .fallback     = '?',
};

const GfxFont* gfx_font_default(void) {
    return &gfx_default_font;
}

int32_t gfx_text_width_n(const GfxFont* font, const char* text, size_t length) {
    if (font == NULL) font = gfx_font_default();
    if (text == NULL) return 0;
    return (int32_t)(strnlen(text, length) * font->advance);
}

int32_t gfx_text_width(const GfxFont* font, const char* text) {
    return gfx_text_width_n(font, text, (size_t)-1);
}

static inline const uint8_t* gfx_font_glyph(const GfxFont* font, uint8_t ch) {
    uint32_t index = (uint32_t)ch - font->first;
    if (ch < font->first || index >= font->count) {
        index = (uint32_t)font->fallback - font->first;
        if (font->fallback < font->first || index >= font->count) return NULL;
    }
    return &font->glyphs[index * font->glyph_width];
}

// Device coordinates (x, y) = top-left of the cell
static void gfx_glyph(GfxContext* ctx, const GfxRect* clip, const GfxFont* font,
                      int32_t x, int32_t y, uint8_t ch, gfx_color_t fg, gfx_color_t bg, bool opaque) {
    int32_t scale  = gfx_get_text_scale(ctx);
    int32_t cell_w = (opaque ? font->advance : font->glyph_width) * scale;
    int32_t cell_h = (opaque ? font->line_height : font->glyph_height) * scale;

    GfxRect cell = gfx_rect(x, y, cell_w, cell_h);
    GfxRect d;
    if (!gfx_rect_intersect(&cell, clip, &d)) return;

    const uint8_t* glyph = gfx_font_glyph(font, ch);
    bool blend = (ctx->state.blend == GFX_BLEND_ALPHA);

    for (int32_t py = d.y; py < d.y + d.h; py++) {
        int32_t row = (py - y) / scale;
        uint32_t* dest = gfx_row(ctx->target, 0, py);

        for (int32_t px = d.x; px < d.x + d.w; px++) {
            int32_t col = (px - x) / scale;
            bool on = glyph != NULL && col < font->glyph_width && row < font->glyph_height &&
                      ((glyph[col] >> row) & 1);

            gfx_color_t color;
            if (on)          color = fg;
            else if (opaque) color = bg;
            else             continue;

            dest[px] = blend ? gfx_blend_pixel(color, dest[px]) : color;
        }
    }
}

static void gfx_text_internal(GfxContext* ctx, int32_t x, int32_t y, const char* text, size_t length,
                              gfx_color_t fg, gfx_color_t bg, bool opaque) {
    GfxRect clip;
    if (text == NULL || !gfx_device_clip(ctx, &clip)) return;

    const GfxFont* font = gfx_get_font(ctx);
    int32_t advance = gfx_context_char_width(ctx);
    x += ctx->state.origin_x;
    y += ctx->state.origin_y;

    // Whole line above, below or right of the clip: nothing to do
    if (y >= clip.y + clip.h || y + gfx_context_line_height(ctx) <= clip.y || x >= clip.x + clip.w) return;

    for (size_t i = 0; i < length && text[i] != '\0'; i++) {
        int32_t cx = x + (int32_t)i * advance;
        if (cx >= clip.x + clip.w) break;
        if (cx + advance <= clip.x) continue;
        gfx_glyph(ctx, &clip, font, cx, y, (uint8_t)text[i], fg, bg, opaque);
    }
}

void gfx_draw_text(GfxContext* ctx, int32_t x, int32_t y, const char* text, gfx_color_t color) {
    gfx_text_internal(ctx, x, y, text, (size_t)-1, color, 0, false);
}

void gfx_draw_text_n(GfxContext* ctx, int32_t x, int32_t y, const char* text, size_t length, gfx_color_t color) {
    gfx_text_internal(ctx, x, y, text, length, color, 0, false);
}

void gfx_draw_text_opaque(GfxContext* ctx, int32_t x, int32_t y, const char* text,
                          gfx_color_t fg, gfx_color_t bg) {
    gfx_text_internal(ctx, x, y, text, (size_t)-1, fg, bg, true);
}

void gfx_draw_char(GfxContext* ctx, int32_t x, int32_t y, uint8_t ch, gfx_color_t fg) {
    char text[2] = { (char)ch, '\0' };
    if (ch == 0) return;
    gfx_text_internal(ctx, x, y, text, 1, fg, 0, false);
}

void gfx_draw_char_opaque(GfxContext* ctx, int32_t x, int32_t y, uint8_t ch, gfx_color_t fg, gfx_color_t bg) {
    GfxRect clip;
    if (!gfx_device_clip(ctx, &clip)) return;

    // Not through the string path: '\0' is a valid cell (the console's blank)
    gfx_glyph(ctx, &clip, gfx_get_font(ctx), x + ctx->state.origin_x, y + ctx->state.origin_y,
              ch, fg, bg, true);
}
