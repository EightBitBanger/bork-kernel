/*
 * Load <home>/sys/images/icon_folder and draw it in the middle of the screen.
 *
 * The file is a resource file holding a sprite, read here in user space the
 * same way the kernel's loader reads it:
 *
 *   resource header (16 bytes)                      resource_manager
 *     0  "RSRC"   4  u16 version (1)   6  u16 type (1 = sprite)
 *     8  u32 payload size              12 u32 reserved
 *   sprite payload                                  classes/image.c
 *     0  u16 width   2  u16 height   4  u16 palette_size
 *     6  u16[]  palette_size * 2 words of palette, then width * height indices
 *
 * The sprite is expanded to 0xAARRGGBB pixels as sprite_get_bitmap() does,
 * then drawn with os/gfx.h the way the window manager draws icons: alpha
 * blended, with opaque black (SYS_GFX_SPRITE_KEY) see-through.
 *
 * Needs SEC_FILE_READ and SEC_GFX (SEC_GFX_SCREEN too when no window manager
 * takes the surface), and SEC_CONSOLE for its messages.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/gfx.h>
#include <os/os.h>

#define ICON_NAME           "icon_folder"

#define RESOURCE_MAGIC      "RSRC"
#define RESOURCE_HEADER     16u
#define RESOURCE_VERSION    1u
#define RESOURCE_SPRITE     1u      // RESOURCE_TYPE_SPRITE

// classes/image.c
#define SPRITE_HEADER_SIZE  6u
#define SPRITE_MAX_PAYLOAD  (16u * 1024u * 1024u)
#define SPRITE_MAX_WORDS    ((SPRITE_MAX_PAYLOAD - SPRITE_HEADER_SIZE) / 2u)

#define PADDING             16      // Around the icon on its panel
#define SHOW_MS             5000    // How long it stays up
#define FRAME_MS            100     // Re-present this often (survives repaints)

// classes/sprite.h
struct Sprite {
    uint16_t width;
    uint16_t height;
    uint16_t palette_size;
    const uint16_t data[];
};

static uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t* p) { return (uint32_t)get_u16(p) | ((uint32_t)get_u16(p + 2) << 16); }

static bool read_exact(FILE* f, void* buf, size_t n) {
    return fread(buf, 1, n, f) == n;
}

//
// Loader: resource header, then image.c's sprite_read()
//

static struct Sprite* sprite_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    struct Sprite* sprite = NULL;
    uint8_t header[RESOURCE_HEADER];

    if (!read_exact(f, header, sizeof(header)) ||
        memcmp(header, RESOURCE_MAGIC, 4) != 0 ||
        get_u16(&header[4]) != RESOURCE_VERSION ||
        get_u16(&header[6]) != RESOURCE_SPRITE) {
        errno = ENOEXEC;
        goto done;
    }
    uint32_t payload_size = get_u32(&header[8]);

    uint8_t sh[SPRITE_HEADER_SIZE];
    if (payload_size < SPRITE_HEADER_SIZE || !read_exact(f, sh, sizeof(sh))) {
        errno = ENOEXEC;
        goto done;
    }

    uint16_t width        = get_u16(&sh[0]);
    uint16_t height       = get_u16(&sh[2]);
    uint16_t palette_size = get_u16(&sh[4]);
    if (width == 0 || height == 0 || palette_size == 0) {
        errno = ENOEXEC;
        goto done;
    }

    uint32_t words = (uint32_t)palette_size * 2u + (uint32_t)width * height;
    uint32_t data_bytes = words * 2u;
    if (words > SPRITE_MAX_WORDS || payload_size != SPRITE_HEADER_SIZE + data_bytes) {
        errno = ENOEXEC;
        goto done;
    }

    sprite = (struct Sprite*)malloc(sizeof(struct Sprite) + data_bytes);
    if (sprite == NULL)
        goto done;
    sprite->width        = width;
    sprite->height       = height;
    sprite->palette_size = palette_size;

    // Read the words, then decode little-endian in place (word i sits in
    // bytes 2i and 2i+1, which are read before they are overwritten)
    uint16_t* data = (uint16_t*)sprite->data;
    uint8_t*  raw  = (uint8_t*)data;
    if (!read_exact(f, raw, data_bytes)) {
        free(sprite);
        sprite = NULL;
        errno = ENOEXEC;    // Cut short
        goto done;
    }
    for (uint32_t i = 0; i < words; i++)
        data[i] = get_u16(&raw[i * 2u]);

done:
    fclose(f);
    return sprite;
}

// sprite.c's sprite_get_bitmap(). Each palette entry is two words, low
// half first, so it is rebuilt explicitly (no unaligned uint32_t reads).
static void sprite_get_bitmap(uint32_t* bitmap, const struct Sprite* sprite) {
    const uint16_t* palette = sprite->data;
    const uint16_t* pixels  = &sprite->data[sprite->palette_size * 2u];
    size_t total = (size_t)sprite->width * sprite->height;

    for (size_t i = 0; i < total; i++) {
        uint16_t index = pixels[i];
        bitmap[i] = (index < sprite->palette_size)
                  ? (uint32_t)palette[index * 2u] | ((uint32_t)palette[index * 2u + 1u] << 16)
                  : 0x00000000u;
    }
}

int main(void) {
    char path[PATH_MAX];
    if (os_syspath("images/" ICON_NAME, path, sizeof(path)) == NULL) {
        perror("icon: no home device");
        return 1;
    }

    struct Sprite* sprite = sprite_load(path);
    if (sprite == NULL) {
        printf("icon: cannot load %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("icon: %s, %ux%u, %u colors\n", path,
           (unsigned)sprite->width, (unsigned)sprite->height, (unsigned)sprite->palette_size);

    int32_t w = sprite->width, h = sprite->height;
    if (w + 2 * PADDING > GFX_SIZE_MAX || h + 2 * PADDING > GFX_SIZE_MAX) {
        printf("icon: %dx%d is too large to draw\n", w, h);
        free(sprite);
        return 1;
    }

    uint32_t* bitmap = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
    if (bitmap == NULL) {
        perror("icon: bitmap");
        free(sprite);
        return 1;
    }
    sprite_get_bitmap(bitmap, sprite);
    free(sprite);

    gfx_info_t info;
    if (gfx_get_info(&info) < 0 || info.width == 0) {
        perror("icon: no display");
        free(bitmap);
        return 1;
    }

    // A panel a little larger than the icon, so its see-through pixels show
    // something other than the surface's initial black
    int32_t pw = w + 2 * PADDING, ph = h + 2 * PADDING;
    gfx_surface_t* s = gfx_surface_create(pw, ph);
    if (s == NULL) {
        perror("icon: gfx_surface_create");
        free(bitmap);
        return 1;
    }

    gfx_fill_gradient(s, 0, 0, pw, ph, GFX_RGB(52, 78, 110), GFX_RGB(26, 38, 58), GFX_GRADIENT_VERTICAL);
    gfx_draw_rect(s, 0, 0, pw, ph, GFX_RGB(140, 170, 200));

    // As the desktop draws icons: blend by alpha, skip the key color.
    // gfx_draw_pixels reads the bitmap before it returns.
    gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_draw_pixels_keyed(s, bitmap, w, h, 0, PADDING, PADDING, SYS_GFX_SPRITE_KEY);
    gfx_set_blend(s, GFX_BLEND_COPY);
    free(bitmap);

    int32_t x = ((int32_t)info.width  - pw) / 2;
    int32_t y = ((int32_t)info.height - ph) / 2;

    int rc = 0;
    for (int elapsed = 0; elapsed < SHOW_MS; elapsed += FRAME_MS) {
        if (gfx_present(s, x, y) < 0) {
            perror("icon: gfx_present");
            rc = 1;
            break;
        }
        os_sleep_ms(FRAME_MS);
    }

    gfx_surface_destroy(s);
    return rc;
}
