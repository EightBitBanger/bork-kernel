/*
 * Fonts (os/font.h): stb_truetype behind a checked loader and a glyph cache.
 *
 * Loading. stb_truetype trusts the file it is given: it follows offsets
 * from the font's tables without checking them against the file size. So
 * before a font goes to stbtt_InitFont, the sfnt layout it relies on is
 * checked here:
 *
 *   table directory    inside the file, every table inside the file
 *   head hhea maxp     present and long enough for the fields stb reads
 *   cmap hmtx          present; hmtx long enough for every glyph
 *   glyf + loca        loca long enough, its offsets in order and inside
 *                      glyf (TrueType outlines)
 *   CFF                present instead of glyf (OpenType "OTTO" outlines;
 *                      the CFF data itself is not checked further)
 *   cmap subtable      the one stb picked: a format it reads (0, 4, 6, 12,
 *                      13), with every array and range inside the table
 *
 * Glyph numbers a cmap gives back are checked against the glyph count by
 * font_glyph(), since stb reads hmtx past its end for larger ones.
 *
 * Rendering. A string is laid out with a floating-point pen: each glyph
 * starts at the pen rounded to a whole pixel, and the pen moves on by the
 * glyph's advance, so widths, fits and drawing agree. Glyphs are rasterized
 * into 8-bit coverage once per face and kept in an open-addressing table
 * keyed by code point. Drawing builds one 0xAARRGGBB buffer the size of the
 * ink (coverage times the color's alpha, the color's RGB everywhere) and
 * hands it to gfx_draw_pixels() alpha blended.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/font.h>

#include <stb_truetype.h>     // Declarations only: stbtruetype.c has the code

#define BUFFER_MAX_W    4096        // Widest string drawn in one piece (pixels)

struct font {
    uint8_t*       data;
    size_t         size;
    stbtt_fontinfo info;
    int            glyphs;
    int            ascent, descent;         // Font units
    char           family[FONT_FAMILY_MAX];
};

typedef struct {
    uint32_t key;                   // Code point + 1; 0: free slot
    int16_t  x0, y0;                // Bitmap corner from the pen / baseline
    uint16_t w, h;
    float    advance;
    uint8_t* bitmap;                // w * h coverage; NULL for no ink
} glyph_t;

struct font_face {
    font_t*  font;
    int32_t  px;
    float    scale;
    int32_t  ascent_px, line_px, cell_w;
    glyph_t* cache;
    size_t   cache_cap, cache_used;
};

// Drawing buffers, shared by every face (one thread)
static uint32_t* pixels;
static size_t    pixels_cap;

typedef struct {
    int32_t        x;               // Pen, rounded
    const glyph_t* g;
} placed_t;

static placed_t* placed;
static size_t    placed_cap;

static uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get_u32(const uint8_t* p) { return (uint32_t)get_u16(p) << 16 | get_u16(p + 2); }

//
// Checking the sfnt layout
//

typedef struct {
    uint32_t offset;
    uint32_t length;
    bool     found;
} table_t;

static table_t find_table(const uint8_t* data, uint32_t font, uint16_t tables, const char* tag) {
    table_t t = { 0, 0, false };
    for (uint16_t i = 0; i < tables; i++) {
        const uint8_t* e = data + font + 12u + 16u * i;
        if (memcmp(e, tag, 4) == 0) {
            t.offset = get_u32(e + 8);
            t.length = get_u32(e + 12);
            t.found  = true;
            break;
        }
    }
    return t;
}

// The font starting at `font` (0, or a collection's entry) is safe to hand
// to stbtt_InitFont
static bool sfnt_ok(const uint8_t* data, size_t size, uint32_t font) {
    if (font > size || size - font < 12u)
        return false;

    uint32_t version = get_u32(data + font);
    bool cff = (version == 0x4F54544Fu);                // "OTTO"
    if (version != 0x00010000u && version != 0x74727565u && !cff)   // 1.0, "true"
        return false;

    uint16_t tables = get_u16(data + font + 4);
    if (tables == 0 || (size - font - 12u) / 16u < tables)
        return false;

    // Every table inside the file
    for (uint16_t i = 0; i < tables; i++) {
        const uint8_t* e = data + font + 12u + 16u * i;
        uint32_t off = get_u32(e + 8), len = get_u32(e + 12);
        if (off > size || len > size - off)
            return false;
    }

    table_t head = find_table(data, font, tables, "head");
    table_t hhea = find_table(data, font, tables, "hhea");
    table_t maxp = find_table(data, font, tables, "maxp");
    table_t hmtx = find_table(data, font, tables, "hmtx");
    table_t cmap = find_table(data, font, tables, "cmap");
    if (!head.found || head.length < 54u || !hhea.found || hhea.length < 36u ||
        !maxp.found || maxp.length < 6u || !hmtx.found || !cmap.found || cmap.length < 4u)
        return false;

    uint32_t glyphs  = get_u16(data + maxp.offset + 4);
    uint32_t metrics = get_u16(data + hhea.offset + 34);
    if (glyphs == 0 || metrics == 0 || metrics > glyphs ||
        hmtx.length < 4u * metrics + 2u * (glyphs - metrics))
        return false;

    if (cff)
        return find_table(data, font, tables, "CFF ").found;

    table_t glyf = find_table(data, font, tables, "glyf");
    table_t loca = find_table(data, font, tables, "loca");
    if (!glyf.found || !loca.found)
        return false;

    // loca: glyphs + 1 offsets into glyf, short (words / 2) or long
    bool wide = get_u16(data + head.offset + 50) != 0;
    if (loca.length / (wide ? 4u : 2u) < glyphs + 1u)
        return false;
    uint32_t last = 0;
    for (uint32_t i = 0; i <= glyphs; i++) {
        const uint8_t* p = data + loca.offset + i * (wide ? 4u : 2u);
        uint32_t at = wide ? get_u32(p) : 2u * get_u16(p);
        if (at < last || at > glyf.length)
            return false;
        last = at;
    }
    return true;
}

// The cmap subtable stb chose (info->index_map) only leads inside cmap
static bool cmap_ok(const uint8_t* data, const stbtt_fontinfo* info) {
    // sfnt_ok() checked the table itself; find it again for its bounds
    uint16_t tables = get_u16(data + info->fontstart + 4);
    table_t cmap = find_table(data, (uint32_t)info->fontstart, tables, "cmap");
    uint32_t end = cmap.offset + cmap.length;
    uint32_t at  = (uint32_t)info->index_map;
    if (at < cmap.offset || at > end || end - at < 8u)
        return false;
    uint32_t room = end - at;

    switch (get_u16(data + at)) {
    case 0:     // Byte encoding: length covers the 256 entries stb may read
        return get_u16(data + at + 2) >= 6u && get_u16(data + at + 2) <= room;
    case 6: {   // Trimmed table
        if (room < 10u)
            return false;
        uint32_t entries = get_u16(data + at + 8);
        return 10u + 2u * entries <= room;
    }
    case 4: {   // Segments, binary searched with the header's own steps
        if (room < 14u)
            return false;
        uint32_t segs     = get_u16(data + at + 6) / 2u;
        uint32_t range    = get_u16(data + at + 8) / 2u;
        uint32_t selector = get_u16(data + at + 10);
        uint32_t shift    = get_u16(data + at + 12) / 2u;
        if (segs == 0 || 16u + 8u * segs > room || selector > 15u ||
            range != (1u << selector) || range > segs || shift > segs - range)
            return false;
        const uint8_t* ends    = data + at + 14u;
        const uint8_t* starts  = ends + 2u * segs + 2u;
        const uint8_t* offsets = ends + 6u * segs + 2u;
        for (uint32_t i = 0; i < segs; i++) {
            uint32_t first = get_u16(starts + 2u * i), last = get_u16(ends + 2u * i);
            uint32_t offset = get_u16(offsets + 2u * i);
            if (first > last)
                return false;
            // idRangeOffset: glyph numbers counted from its own position
            uint32_t from = (uint32_t)(offsets - data) + 2u * i;
            if (offset != 0 && (uint64_t)from + offset + 2u * (last - first) + 2u > end)
                return false;
        }
        return true;
    }
    case 12:
    case 13: {  // Groups of 12 bytes
        if (room < 16u)
            return false;
        uint32_t groups = get_u32(data + at + 12);
        return groups <= (room - 16u) / 12u;
    }
    default:    // stb asserts on format 2 and anything newer
        return false;
    }
}


//
// Opening
//

// The family name (name ID 1) as ASCII: Windows Unicode first, then Mac Roman
static void read_family(font_t* font) {
    static const struct { int platform, encoding, language; } tries[] = {
        { STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH },
        { STBTT_PLATFORM_ID_MAC,       STBTT_MAC_EID_ROMAN,      STBTT_MAC_LANG_ENGLISH },
    };
    font->family[0] = '\0';

    for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
        int length = 0;
        const char* s = stbtt_GetFontNameString(&font->info, &length, tries[t].platform,
                                                tries[t].encoding, tries[t].language, 1);
        const uint8_t* p = (const uint8_t*)s;
        if (p == NULL || length <= 0 || p < font->data ||
            (size_t)length > font->size - (size_t)(p - font->data))
            continue;

        // UTF-16BE: one character per two bytes; anything past ASCII is '?'
        size_t step = (tries[t].platform == STBTT_PLATFORM_ID_MICROSOFT) ? 2 : 1;
        size_t n = 0;
        for (size_t i = 0; i + step <= (size_t)length && n < FONT_FAMILY_MAX - 1; i += step) {
            uint32_t c = (step == 2) ? get_u16(p + i) : p[i];
            font->family[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        font->family[n] = '\0';
        if (n > 0)
            return;
    }
}

// Takes `data` (malloc'd): freed on failure, kept by the font otherwise
static font_t* open_owned(uint8_t* data, size_t size) {
    font_t* font = (font_t*)calloc(1, sizeof(font_t));
    if (font == NULL) {
        free(data);
        return NULL;
    }

    // A collection ("ttcf") lists its fonts; take the first
    uint32_t offset = 0;
    bool ok = size >= 12;
    if (ok && memcmp(data, "ttcf", 4) == 0) {
        ok = size >= 16 && get_u32(data + 8) != 0;
        if (ok)
            offset = get_u32(data + 12);
    }
    if (!ok || !sfnt_ok(data, size, offset) || !stbtt_InitFont(&font->info, data, (int)offset) ||
        !cmap_ok(data, &font->info)) {
        free(font);
        free(data);
        errno = ENOEXEC;
        return NULL;
    }

    font->data   = data;
    font->size   = size;
    font->glyphs = font->info.numGlyphs;
    int gap;
    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &gap);
    read_family(font);
    return font;
}

font_t* font_open(const void* data, size_t size) {
    if (data == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (size > FONT_FILE_MAX) {
        errno = EFBIG;
        return NULL;
    }
    uint8_t* copy = (uint8_t*)malloc(size ? size : 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, data, size);
    return open_owned(copy, size);
}

font_t* font_open_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    uint8_t* data = NULL;
    long length;
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
        goto done;
    if ((unsigned long)length > FONT_FILE_MAX) {
        errno = EFBIG;
        goto done;
    }
    data = (uint8_t*)malloc(length ? (size_t)length : 1);
    if (data == NULL)
        goto done;
    if (fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
        errno = EIO;
    }

done:
    fclose(f);
    return data != NULL ? open_owned(data, (size_t)length) : NULL;
}

void font_close(font_t* font) {
    if (font == NULL)
        return;
    free(font->data);
    free(font);
}

const char* font_family(const font_t* font) {
    return font != NULL ? font->family : "";
}

const void* font_bytes(const font_t* font, size_t* size) {
    if (size != NULL)
        *size = font != NULL ? font->size : 0;
    return font != NULL ? font->data : NULL;
}

int font_glyph_count(const font_t* font) {
    return font != NULL ? font->glyphs : 0;
}

int font_glyph(const font_t* font, uint32_t codepoint) {
    if (font == NULL || codepoint > 0x10FFFFu)
        return 0;
    int glyph = stbtt_FindGlyphIndex(&font->info, (int)codepoint);
    return (glyph > 0 && glyph < font->glyphs) ? glyph : 0;
}

bool font_has_glyph(const font_t* font, uint32_t codepoint) {
    return font_glyph(font, codepoint) != 0;
}

//
// UTF-8
//

// The code point at text[*i], moving *i past it. A byte that doesn't start
// a valid sequence is one '?'.
static uint32_t next_char(const char* text, size_t len, size_t* i) {
    const uint8_t* p = (const uint8_t*)text + *i;
    size_t left = len - *i;
    uint32_t c = p[0];
    size_t n;
    uint32_t min;

    if (c < 0x80)                { *i += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { n = 2; min = 0x80;    c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; min = 0x800;   c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; min = 0x10000; c &= 0x07; }
    else                         { *i += 1; return '?'; }

    if (left < n) {
        *i += 1;
        return '?';
    }
    for (size_t k = 1; k < n; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *i += 1;
            return '?';
        }
        c = (c << 6) | (p[k] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *i += 1;
        return '?';
    }
    *i += n;
    return c;
}

static int32_t round_px(float v) {
    return (int32_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

static int32_t ceil_px(float v) {
    int32_t i = (int32_t)v;
    return (v > (float)i) ? i + 1 : i;
}

//
// Glyph cache
//

static void cache_clear(font_face_t* face) {
    for (size_t i = 0; i < face->cache_cap; i++)
        free(face->cache[i].bitmap);
    free(face->cache);
    face->cache = NULL;
    face->cache_cap = face->cache_used = 0;
}

static size_t slot_of(const glyph_t* table, size_t cap, uint32_t key) {
    size_t at = (key * 2654435761u) & (cap - 1);
    while (table[at].key != 0 && table[at].key != key)
        at = (at + 1) & (cap - 1);
    return at;
}

static bool cache_grow(font_face_t* face) {
    size_t cap = face->cache_cap ? face->cache_cap * 2 : 256;
    glyph_t* table = (glyph_t*)calloc(cap, sizeof(glyph_t));
    if (table == NULL)
        return false;
    for (size_t i = 0; i < face->cache_cap; i++)
        if (face->cache[i].key != 0)
            table[slot_of(table, cap, face->cache[i].key)] = face->cache[i];
    free(face->cache);
    face->cache = table;
    face->cache_cap = cap;
    return true;
}

static void rasterize(const font_face_t* face, glyph_t* g, uint32_t c) {
    const stbtt_fontinfo* info = &face->font->info;
    int glyph = font_glyph(face->font, c);
    float scale = face->scale;

    int advance, lsb;
    stbtt_GetGlyphHMetrics(info, glyph, &advance, &lsb);
    g->advance = (float)advance * scale;

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    // A damaged glyph box would ask for a huge bitmap: no ink instead
    int limit = 4 * face->px + 16;
    if (w <= 0 || h <= 0 || w > limit || h > limit ||
        x0 < -limit || x0 > limit || y0 < -limit || y0 > limit)
        return;

    g->bitmap = (uint8_t*)malloc((size_t)w * (size_t)h);
    if (g->bitmap == NULL)
        return;
    stbtt_MakeGlyphBitmap(info, g->bitmap, w, h, w, scale, scale, glyph);
    g->x0 = (int16_t)x0;
    g->y0 = (int16_t)y0;
    g->w  = (uint16_t)w;
    g->h  = (uint16_t)h;
}

// The glyph for c (control characters are spaces). Valid until the next
// call: the table may move.
static const glyph_t* glyph_for(font_face_t* face, uint32_t c) {
    static glyph_t none;            // Out of memory: no ink, no advance

    if (c < 0x20 || c == 0x7F)
        c = ' ';
    uint32_t key = c + 1;
    if (face->cache_cap > 0) {
        size_t at = slot_of(face->cache, face->cache_cap, key);
        if (face->cache[at].key == key)
            return &face->cache[at];
    }
    if ((face->cache_used + 1) * 4 > face->cache_cap * 3 && !cache_grow(face))
        return &none;

    glyph_t* g = &face->cache[slot_of(face->cache, face->cache_cap, key)];
    memset(g, 0, sizeof(*g));
    g->key = key;
    rasterize(face, g, c);
    face->cache_used++;
    return g;
}

//
// Faces
//

font_face_t* font_face_create(font_t* font, int32_t px) {
    if (font == NULL) {
        errno = EINVAL;
        return NULL;
    }
    font_face_t* face = (font_face_t*)calloc(1, sizeof(font_face_t));
    if (face == NULL)
        return NULL;

    if (px < FONT_PX_MIN) px = FONT_PX_MIN;
    if (px > FONT_PX_MAX) px = FONT_PX_MAX;
    face->font      = font;
    face->px        = px;
    face->scale     = stbtt_ScaleForPixelHeight(&font->info, (float)px);
    face->ascent_px = round_px((float)font->ascent * face->scale);
    face->line_px   = face->ascent_px + round_px((float)-font->descent * face->scale);
    if (face->line_px < 1)
        face->line_px = px;
    face->cell_w = round_px(glyph_for(face, '0')->advance);
    if (face->cell_w < 1)
        face->cell_w = (px + 1) / 2;
    return face;
}

void font_face_destroy(font_face_t* face) {
    if (face == NULL)
        return;
    cache_clear(face);
    free(face);
}

font_t*  font_face_font(const font_face_t* face)       { return face->font; }
int32_t  font_face_px(const font_face_t* face)         { return face->px; }
int32_t  font_face_height(const font_face_t* face)     { return face->line_px; }
int32_t  font_face_ascent(const font_face_t* face)     { return face->ascent_px; }
int32_t  font_face_cell_width(const font_face_t* face) { return face->cell_w; }

//
// Measuring
//

int32_t font_text_width(font_face_t* face, const char* text, size_t len) {
    if (face == NULL || text == NULL)
        return 0;
    float pen = 0;
    for (size_t i = 0; i < len; )
        pen += glyph_for(face, next_char(text, len, &i))->advance;
    return ceil_px(pen);
}

size_t font_text_fit(font_face_t* face, const char* text, size_t len, int32_t width) {
    if (face == NULL || text == NULL || width <= 0)
        return 0;
    float pen = 0;
    size_t i = 0;
    while (i < len) {
        size_t at = i;
        pen += glyph_for(face, next_char(text, len, &i))->advance;
        if (ceil_px(pen) > width)
            return at;
    }
    return len;
}

//
// Drawing
//

static bool reserve_placed(size_t n) {
    if (n <= placed_cap)
        return true;
    placed_t* p = (placed_t*)realloc(placed, n * sizeof(placed_t));
    if (p == NULL)
        return false;
    placed = p;
    placed_cap = n;
    return true;
}

// Draw the `n` glyphs in `placed` (pens relative to x), line top at y
static void draw_placed(font_face_t* face, gfx_surface_t* s, int32_t x, int32_t y, size_t n,
                        gfx_color_t color) {
    // The ink's extent
    int32_t left = 0, right = 0, top = 0, bottom = face->line_px;
    for (size_t i = 0; i < n; i++) {
        const glyph_t* g = placed[i].g;
        if (g->bitmap == NULL)
            continue;
        int32_t gx = placed[i].x + g->x0, gy = face->ascent_px + g->y0;
        if (gx < left)              left   = gx;
        if (gx + g->w > right)      right  = gx + g->w;
        if (gy < top)               top    = gy;
        if (gy + g->h > bottom)     bottom = gy + g->h;
    }
    if (right > BUFFER_MAX_W)
        right = BUFFER_MAX_W;
    int32_t bw = right - left, bh = bottom - top;
    if (bw <= 0 || bh <= 0)
        return;

    size_t need = (size_t)bw * (size_t)bh;
    if (need > pixels_cap) {
        uint32_t* p = (uint32_t*)realloc(pixels, need * sizeof(uint32_t));
        if (p == NULL)
            return;
        pixels = p;
        pixels_cap = need;
    }

    // Coverage * alpha into the buffer; where glyphs overlap the larger wins
    uint32_t rgb = color & 0x00FFFFFFu, alpha = color >> 24;
    for (size_t i = 0; i < need; i++)
        pixels[i] = rgb;
    for (size_t i = 0; i < n; i++) {
        const glyph_t* g = placed[i].g;
        if (g->bitmap == NULL)
            continue;
        int32_t gx = placed[i].x + g->x0 - left, gy = face->ascent_px + g->y0 - top;
        for (int32_t row = 0; row < g->h; row++) {
            uint32_t* out = pixels + (size_t)(gy + row) * (size_t)bw;
            const uint8_t* in = g->bitmap + (size_t)row * g->w;
            for (int32_t col = 0; col < g->w; col++) {
                int32_t px = gx + col;
                if (px < 0 || px >= bw || in[col] == 0)
                    continue;
                uint32_t a = (in[col] * alpha + 127) / 255;
                if (a > (out[px] >> 24))
                    out[px] = (a << 24) | rgb;
            }
        }
    }

    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_draw_pixels(s, pixels, bw, bh, 0, x + left, y + top);
    gfx_set_blend(s, old);
}

void font_draw_text(font_face_t* face, gfx_surface_t* s, int32_t x, int32_t y,
                    const char* text, size_t len, gfx_color_t color) {
    if (face == NULL || s == NULL || text == NULL || len == 0 || !reserve_placed(len))
        return;

    // Fill the cache first: later lookups don't move the table
    for (size_t i = 0; i < len; )
        glyph_for(face, next_char(text, len, &i));

    size_t n = 0;
    float pen = 0;
    for (size_t i = 0; i < len; ) {
        const glyph_t* g = glyph_for(face, next_char(text, len, &i));
        placed[n].x = round_px(pen);
        placed[n].g = g;
        n++;
        pen += g->advance;
    }
    draw_placed(face, s, x, y, n, color);
}

void font_draw_cells(font_face_t* face, gfx_surface_t* s, int32_t x, int32_t y,
                     const uint32_t* codepoints, size_t count, int32_t cell_w,
                     gfx_color_t color) {
    if (face == NULL || s == NULL || codepoints == NULL || count == 0 || !reserve_placed(count))
        return;

    for (size_t i = 0; i < count; i++)
        if (codepoints[i] != 0)
            glyph_for(face, codepoints[i]);

    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (codepoints[i] == 0)
            continue;
        const glyph_t* g = glyph_for(face, codepoints[i]);
        placed[n].x = (int32_t)i * cell_w + round_px(((float)cell_w - g->advance) / 2);
        placed[n].g = g;
        n++;
    }
    draw_placed(face, s, x, y, n, color);
}
