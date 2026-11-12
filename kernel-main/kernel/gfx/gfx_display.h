#ifndef _KERNEL_GFX_DISPLAY_H_
#define _KERNEL_GFX_DISPLAY_H_

// The physical display
//
// Two surfaces: the FRONT surface is the hardware framebuffer (VRAM, mapped
// write-combining, stride = the hardware pitch) and the BACK surface is a
// packed copy in RAM (stride = width). Everything is drawn into the back
// surface through a GfxContext; gfx_display_present() copies the parts
// that changed to VRAM.
//
// Nobody draws into the front surface directly (except the panic screen,
// see gfx_display_front).

#include <kernel/gfx/gfx.h>

// Call once, from kmain, before anything draws. Addresses are kernel
// virtual addresses; pitch is in bytes. The front buffer may be mapped
// later, as long as nothing is presented before it is.
void gfx_display_init(uint32_t front_address, uint32_t pitch_bytes,
                      uint32_t back_address, int32_t width, int32_t height);

bool gfx_display_ready(void);

int32_t gfx_display_width(void);
int32_t gfx_display_height(void);

// The composition target. Wrap it in your own GfxContext to draw.
GfxSurface* gfx_display_back(void);

// The hardware framebuffer itself. For the panic screen ONLY: it must show
// something with the rest of the kernel in an unknown state, so it draws
// straight to VRAM instead of trusting the back buffer and a present.
// Everyone else draws into the back surface.
GfxSurface* gfx_display_front(void);

// Copy part of the back surface to VRAM (cut to the screen)
void gfx_display_present(const GfxRect* rect);
void gfx_display_present_rect(int32_t x, int32_t y, int32_t w, int32_t h);
void gfx_display_present_all(void);

#endif
