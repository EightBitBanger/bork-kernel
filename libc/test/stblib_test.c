/*
 * The stb library (os/stblib.h) and the user library loader (os/ulib.h).
 *
 * Built twice by the Makefile from this one file (dyn with -DSTBLIB_DYN):
 *
 *   build/stblib_ref   stb linked in, as programs have it without
 *                      libc_use_stblib()
 *   build/stblib_dyn   with libc/stblib/stblib_client.c: stb comes from
 *                      <home>/sys/lib/stb, the real library file
 *
 *   stblib_test report [font]   hashes of what stb gives back for the same
 *                               work: images encoded and decoded (PNG, JPEG,
 *                               BMP, TGA, 16-bit), errors, a font's metrics
 *                               and glyphs. Both builds must print the same.
 *   stblib_test loader          (dyn) ulib_open on the real file and on
 *                               damaged copies of it
 *   stblib_test missing         (dyn) no library: everything fails cleanly
 *                               and says why
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/font.h>
#include <os/image.h>
#include <os/stblib.h>
#include <os/ulib.h>

#define STBI_NO_STDIO
#include "../src/stb_image.h"
#define STBI_WRITE_NO_STDIO
#include "../src/stb_image_write.h"

static int checks, failures;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno); } \
} while (0)

static uint32_t fnv(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

// A growing buffer for the *_to_func writers
typedef struct { uint8_t* data; size_t size, cap; } sink_t;

static void put(void* ctx, void* data, int size) {
    sink_t* s = (sink_t*)ctx;
    if (s->size + (size_t)size > s->cap) {
        s->cap = (s->size + (size_t)size) * 2;
        s->data = realloc(s->data, s->cap);
    }
    memcpy(s->data + s->size, data, (size_t)size);
    s->size += (size_t)size;
}

static uint8_t* read_all(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc((size_t)n);
    if (data != NULL && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

//
// report
//

#define W 97
#define H 61

static void report_images(void) {
    // A picture with every channel varying, alpha too
    static uint32_t argb[W * H];
    static uint8_t rgba[W * H * 4];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t r = (uint8_t)(x * 255 / (W - 1)), g = (uint8_t)(y * 255 / (H - 1));
            uint8_t b = (uint8_t)((x * y) & 0xFF), a = (uint8_t)(255 - ((x + y) & 0x7F));
            argb[y * W + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            uint8_t* p = &rgba[(y * W + x) * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = a;
        }
    }

    // os/image.h: PNG round trip through a file
    CHECK(bitmap_save_png("t.png", argb, W, H, 0) == 0);
    size_t png_size = 0;
    uint8_t* png = read_all("t.png", &png_size);
    CHECK(png != NULL);
    printf("png file      %6u bytes  %08x\n", (unsigned)png_size, (unsigned)fnv(png, png_size));
    bitmap_t bm;
    CHECK(bitmap_load("t.png", &bm) == 0);
    CHECK(bm.width == W && bm.height == H);
    CHECK(bm.pixels != NULL && memcmp(bm.pixels, argb, sizeof(argb)) == 0);
    printf("png decoded   %dx%d  %08x\n", bm.width, bm.height,
           bm.pixels ? (unsigned)fnv(bm.pixels, sizeof(argb)) : 0u);
    bitmap_free(&bm);

    // stb_image directly: info, 16-bit
    int x = 0, y = 0, comp = 0;
    CHECK(stbi_info_from_memory(png, (int)png_size, &x, &y, &comp) == 1);
    printf("png info      %dx%d comp %d  16-bit %d\n", x, y, comp, stbi_is_16_bit_from_memory(png, (int)png_size));
    unsigned short* p16 = stbi_load_16_from_memory(png, (int)png_size, &x, &y, &comp, 4);
    CHECK(p16 != NULL);
    printf("png as 16-bit %08x\n", p16 ? (unsigned)fnv(p16, (size_t)W * H * 8) : 0u);
    stbi_image_free(p16);

    // Flip on load: a library global, set through the library
    stbi_set_flip_vertically_on_load(1);
    unsigned char* flipped = stbi_load_from_memory(png, (int)png_size, &x, &y, &comp, 4);
    stbi_set_flip_vertically_on_load(0);
    CHECK(flipped != NULL && memcmp(flipped, &rgba[(H - 1) * W * 4], W * 4) == 0);
    printf("png flipped   %08x\n", flipped ? (unsigned)fnv(flipped, sizeof(rgba)) : 0u);
    free(flipped);      // From the program's heap, as libc's image.c relies on
    free(png);

    // The other writers, decoded back
    static const struct { const char* name; int kind; } formats[] = {
        { "jpeg q90", 0 }, { "bmp", 1 }, { "tga", 2 },
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        sink_t s = { 0 };
        int ok = 0;
        switch (formats[i].kind) {
            case 0: ok = stbi_write_jpg_to_func(put, &s, W, H, 4, rgba, 90); break;
            case 1: ok = stbi_write_bmp_to_func(put, &s, W, H, 4, rgba); break;
            case 2: ok = stbi_write_tga_to_func(put, &s, W, H, 4, rgba); break;
        }
        CHECK(ok && s.size > 0);
        unsigned char* back = stbi_load_from_memory(s.data, (int)s.size, &x, &y, &comp, 4);
        CHECK(back != NULL && x == W && y == H);
        printf("%-13s %6u bytes  %08x  decoded %08x\n", formats[i].name, (unsigned)s.size,
               (unsigned)fnv(s.data, s.size), back ? (unsigned)fnv(back, sizeof(rgba)) : 0u);
        stbi_image_free(back);
        free(s.data);
    }

    // Not pictures: refused, with stb's reason
    static const uint8_t junk[64] = "this is not a picture at all, just some text in a buffer";
    CHECK(bitmap_decode(junk, sizeof(junk), &bm) == -1 && errno == ENOEXEC);
    CHECK(stbi_load_from_memory(junk, sizeof(junk), &x, &y, &comp, 4) == NULL);
    printf("junk          \"%s\"\n", stbi_failure_reason());
    // A PNG cut short: a decode error with a reason
    uint8_t cut[80];
    size_t n = 0;
    uint8_t* again = read_all("t.png", &n);
    memcpy(cut, again, sizeof(cut));
    free(again);
    CHECK(bitmap_decode(cut, sizeof(cut), &bm) == -1);
    printf("cut png       errno %d \"%s\"\n", errno, bitmap_error());
}

static void report_font(const char* path) {
    font_t* font = font_open_file(path);
    CHECK(font != NULL);
    if (font == NULL)
        return;
    printf("font          \"%s\" %d glyphs\n", font_family(font), font_glyph_count(font));
    static const char* text = "Hello, stb library! 0123 @#&";
    for (int px = 9; px <= 48; px += 13) {
        font_face_t* face = font_face_create(font, px);
        CHECK(face != NULL);
        printf("face %2dpx     height %d ascent %d cell %d  width %d  fit(100) %u\n", px,
               font_face_height(face), font_face_ascent(face), font_face_cell_width(face),
               font_text_width(face, text, strlen(text)), (unsigned)font_text_fit(face, text, strlen(text), 100));
        font_face_destroy(face);
    }

    // stb_truetype directly, on the same bytes
    size_t size = 0;
    const uint8_t* data = font_bytes(font, &size);
    stbtt_fontinfo info;
    CHECK(stbtt_GetNumberOfFonts(data) >= 1);
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    CHECK(stbtt_InitFont(&info, data, offset) == 1);
    float scale = stbtt_ScaleForPixelHeight(&info, 32.0f);
    int ascent, descent, gap, x0, y0, x1, y1;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    stbtt_GetFontBoundingBox(&info, &x0, &y0, &x1, &y1);
    printf("metrics       scale %.6f em %.6f  asc %d desc %d gap %d  box %d %d %d %d\n", (double)scale,
           (double)stbtt_ScaleForMappingEmToPixels(&info, 32.0f), ascent, descent, gap, x0, y0, x1, y1);
    printf("kern          AV %d To %d\n", stbtt_GetCodepointKernAdvance(&info, 'A', 'V'),
           stbtt_GetGlyphKernAdvance(&info, stbtt_FindGlyphIndex(&info, 'T'), stbtt_FindGlyphIndex(&info, 'o')));
    for (const char* c = "Ag@ "; *c != '\0'; c++) {
        int glyph = stbtt_FindGlyphIndex(&info, *c), adv, lsb, w, h, xo, yo;
        stbtt_GetGlyphHMetrics(&info, glyph, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&info, glyph, scale, scale, &x0, &y0, &x1, &y1);
        uint8_t out[64 * 64];
        memset(out, 0, sizeof(out));
        stbtt_MakeGlyphBitmap(&info, out, 64, 64, 64, scale, scale, glyph);
        unsigned char* bmp = stbtt_GetCodepointBitmap(&info, scale, scale, *c, &w, &h, &xo, &yo);
        printf("glyph '%c'     #%d adv %d lsb %d empty %d box %d %d %d %d  made %08x  got %dx%d %08x\n", *c, glyph,
               adv, lsb, stbtt_IsGlyphEmpty(&info, glyph), x0, y0, x1, y1, (unsigned)fnv(out, sizeof(out)), w, h,
               bmp ? (unsigned)fnv(bmp, (size_t)w * h) : 0u);
        stbtt_FreeBitmap(bmp, NULL);
    }
    int length = 0;
    const char* name = stbtt_GetFontNameString(&info, &length, 3, 1, 0x409, 4);
    printf("name string   %d bytes %08x\n", length, name ? (unsigned)fnv(name, (size_t)length) : 0u);
    font_close(font);
}

#ifdef STBLIB_DYN

//
// loader (dyn only)
//

static int write_all(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    int ok = fwrite(data, 1, size, f) == size;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

// Copy of the library file with one change, the CRC fixed up again when
// `recrc`, so that the check after the CRC is the one that has to catch it
static const char* damaged(const uint8_t* file, size_t size, size_t at, uint32_t value, int recrc,
                           size_t keep) {
    static char path[] = "damaged";
    uint8_t* copy = malloc(size);
    memcpy(copy, file, size);
    if (size >= 4 && at <= size - 4)
        memcpy(copy + at, &value, 4);
    if (recrc) {
        memset(copy + offsetof(ulib_header_t, checksum), 0, 4);
        uint32_t crc = ulib_crc32(0, copy, (uint32_t)keep);
        memcpy(copy + offsetof(ulib_header_t, checksum), &crc, 4);
    }
    write_all(path, copy, keep);
    free(copy);
    return path;
}

static void expect_refused(const char* path, const char* name, uint32_t abi, int err, const char* why) {
    errno = 0;
    ulib_t* lib = ulib_open(path, name, abi);
    CHECK(lib == NULL);
    CHECK(errno == err);
    if (strstr(ulib_error(), why) == NULL) {
        failures++;
        printf("FAIL reason \"%s\", expected \"%s\"\n", ulib_error(), why);
    }
    checks++;
    printf("refused       %-28s %s\n", why, ulib_error());
    ulib_close(lib);
}

static void loader(void) {
    const char* path = "sys/lib/stb";
    ulib_t* lib = ulib_open(path, STBLIB_NAME, STBLIB_ABI);
    CHECK(lib != NULL);
    if (lib == NULL) {
        printf("ulib_open: %s\n", ulib_error());
        return;
    }
    const stblib_api_t* api = ulib_exports(lib);
    CHECK(api->size == sizeof(stblib_api_t));
    CHECK(strcmp(ulib_name(lib), "stb") == 0);
    CHECK(*ulib_error() == '\0');
    printf("loaded        %s, %u bytes in memory, table %u bytes\n", ulib_name(lib), (unsigned)ulib_size(lib),
           (unsigned)api->size);

    // A second copy is independent: its own globals (the flip flag)
    ulib_t* lib2 = ulib_open(path, NULL, STBLIB_ABI);
    CHECK(lib2 != NULL && ulib_exports(lib2) != ulib_exports(lib));
    const stblib_api_t* api2 = ulib_exports(lib2);
    api2->stbi_set_flip_vertically_on_load(1);
    uint8_t px[2 * 4] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    sink_t s = { 0 };
    CHECK(api->stbi_write_tga_to_func(put, &s, 1, 2, 4, px));
    int x, y, c;
    unsigned char* a = api->stbi_load_from_memory(s.data, (int)s.size, &x, &y, &c, 4);
    unsigned char* b = api2->stbi_load_from_memory(s.data, (int)s.size, &x, &y, &c, 4);
    CHECK(a && b && a[0] == 1 && b[0] == 5);
    free(a); free(b); free(s.data);
    ulib_close(lib2);

    // Load and free it many times: nothing kept between
    for (int i = 0; i < 50; i++) {
        ulib_t* l = ulib_open(path, STBLIB_NAME, STBLIB_ABI);
        CHECK(l != NULL);
        ulib_close(l);
    }

    // Wrong expectations
    expect_refused(path, "png", STBLIB_ABI, ENOEXEC, "a different library");
    expect_refused(path, STBLIB_NAME, STBLIB_ABI + 1, ENOEXEC, "version does not match");
    expect_refused("sys/lib/none", STBLIB_NAME, STBLIB_ABI, ENOENT, "can't open");
    expect_refused(NULL, NULL, 0, EINVAL, "no path");

    // Damaged copies
    size_t size = 0;
    uint8_t* file = read_all(path, &size);
    ulib_header_t h;
    memcpy(&h, file, sizeof(h));
    expect_refused(damaged(file, size, 0, 0x7672646b /* "kdrv" */, 1, size), NULL, STBLIB_ABI, ENOEXEC, "not a library");
    expect_refused(damaged(file, size, h.image_offset + 100, 0x12345678, 0, size), NULL, STBLIB_ABI, ENOEXEC, "bad checksum");
    expect_refused(damaged(file, size, size, 0, 0, size - 10), NULL, STBLIB_ABI, ENOEXEC, "bad checksum");
    expect_refused(damaged(file, size, size, 0, 0, 20), NULL, STBLIB_ABI, ENOEXEC, "too short");
    expect_refused(damaged(file, size, offsetof(ulib_header_t, machine), 62 /* x86-64 */, 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "another CPU");
    expect_refused(damaged(file, size, offsetof(ulib_header_t, version), 2 | (64u << 16), 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "format version");
    expect_refused(damaged(file, size, offsetof(ulib_header_t, host_version), 99, 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "newer libc");
    expect_refused(damaged(file, size, offsetof(ulib_header_t, image_size), (uint32_t)size, 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "image outside the file");
    expect_refused(damaged(file, size, offsetof(ulib_header_t, bss_size), 0xFFFFFF00u, 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "larger than ULIB_IMAGE_MAX");
    expect_refused(damaged(file, size, offsetof(ulib_header_t, reloc_count), h.reloc_count + 1000, 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "relocation list outside");
    expect_refused(damaged(file, size, offsetof(ulib_header_t, entry), h.image_size, 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "entry point outside");
    expect_refused(damaged(file, size, h.reloc_offset + 4 * (h.reloc_count / 2), h.image_size - 2, 1, size), NULL,
                   STBLIB_ABI, ENOEXEC, "relocation outside the image");
    // An entry point that refuses: point it at ulib_entry's "return NULL"
    // can't be arranged from here, but a name that isn't terminated can
    char bad_name[16];
    memset(bad_name, 'x', sizeof(bad_name));
    uint8_t* copy = malloc(size);
    memcpy(copy, file, size);
    memcpy(copy + offsetof(ulib_header_t, name), bad_name, sizeof(bad_name));
    memset(copy + offsetof(ulib_header_t, checksum), 0, 4);
    uint32_t crc = ulib_crc32(0, copy, (uint32_t)size);
    memcpy(copy + offsetof(ulib_header_t, checksum), &crc, 4);
    write_all("damaged", copy, size);
    free(copy);
    expect_refused("damaged", NULL, STBLIB_ABI, ENOEXEC, "bad library name");
    free(file);

    // The real one still loads after all that
    ulib_t* l = ulib_open(path, STBLIB_NAME, STBLIB_ABI);
    CHECK(l != NULL && *ulib_error() == '\0');
    ulib_close(l);
    ulib_close(lib);
}

//
// missing (dyn only)
//

static void missing(void) {
    CHECK(stblib() == NULL);
    printf("stblib_error  \"%s\"\n", stblib_error());
    CHECK(strstr(stblib_error(), "sys/lib/stb") != NULL);
    int x, y, c;
    static const uint8_t data[16] = { 0x89, 'P', 'N', 'G' };
    CHECK(stbi_load_from_memory(data, sizeof(data), &x, &y, &c, 4) == NULL);
    CHECK(stbi_info_from_memory(data, sizeof(data), &x, &y, &c) == 0);
    CHECK(strcmp(stbi_failure_reason(), stblib_error()) == 0);
    bitmap_t bm;
    CHECK(bitmap_decode(data, sizeof(data), &bm) == -1 && errno == ENOEXEC);
    CHECK(strcmp(bitmap_error(), stblib_error()) == 0);
    printf("bitmap_error  \"%s\"\n", bitmap_error());
    static const uint32_t px[4] = { 0 };
    CHECK(bitmap_save_png("t.png", px, 2, 2, 0) == -1);
    stbtt_fontinfo info;
    CHECK(stbtt_InitFont(&info, data, 0) == 0);
    CHECK(stbtt_GetFontOffsetForIndex(data, 0) == -1);
    int a = 7;
    stbtt_GetFontVMetrics(&info, &a, NULL, NULL);       // Does nothing, doesn't crash
    CHECK(a == 7);
}

#endif

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "report";
    if (strcmp(mode, "report") == 0) {
        report_images();
        if (argc > 2)
            report_font(argv[2]);
#ifdef STBLIB_DYN
    } else if (strcmp(mode, "loader") == 0) {
        loader();
    } else if (strcmp(mode, "missing") == 0) {
        missing();
#endif
    } else {
        fprintf(stderr, "usage: stblib_test report [font] | loader | missing\n");
        return 2;
    }
    fprintf(stderr, "stblib_test %s: %d checks, %d failures\n", mode, checks, failures);
    return failures != 0;
}
