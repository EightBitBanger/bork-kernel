// Back buffer, VRAM and presenting

#include <kernel/gfx/gfx_display.h>
#include <kernel/gfx/gfx_internal.h>

#include <kernel/util/string.h>

static GfxSurface display_front;     // VRAM (write-combining), stride = pitch / 4
static GfxSurface display_back;      // RAM, packed (stride = width)
static bool       display_ready = false;

void gfx_display_init(uint32_t front_address, uint32_t pitch_bytes,
                      uint32_t back_address, int32_t width, int32_t height) {
    gfx_surface_wrap(&display_front, (uint32_t*)(uintptr_t)front_address, width, height,
                     (int32_t)(pitch_bytes / sizeof(uint32_t)));
    gfx_surface_wrap(&display_back, (uint32_t*)(uintptr_t)back_address, width, height, width);
    display_ready = (display_front.pixels != NULL && display_back.pixels != NULL);
}

bool gfx_display_ready(void) {
    return display_ready;
}

int32_t gfx_display_width(void) {
    return display_back.width;
}

int32_t gfx_display_height(void) {
    return display_back.height;
}

GfxSurface* gfx_display_back(void) {
    return &display_back;
}

GfxSurface* gfx_display_front(void) {
    return &display_front;
}

void gfx_display_present(const GfxRect* rect) {
    if (!display_ready) return;

    GfxRect screen = gfx_surface_bounds(&display_back);
    GfxRect r;
    if (rect == NULL) r = screen;
    else if (!gfx_rect_intersect(rect, &screen, &r)) return;

    for (int32_t y = r.y; y < r.y + r.h; y++)
        gfx_row_copy(gfx_row(&display_front, r.x, y), gfx_row(&display_back, r.x, y), (size_t)r.w);

    // VRAM is write-combining: drain the WC buffers so the frame shows now
    __asm__ volatile("sfence" ::: "memory");
}

void gfx_display_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    GfxRect r = gfx_rect(x, y, w, h);
    gfx_display_present(&r);
}

void gfx_display_present_all(void) {
    gfx_display_present(NULL);
}
