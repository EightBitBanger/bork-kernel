
#include <kernel/panic/panic_error.h>
#include <stdint.h>
#include <stdbool.h>

#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_display.h>

// Layout & Styling Constants
#define COLOR_BG        0xFF3A0000  // Deep dark red (crash)
#define COLOR_BG_WD     0xFF00203A  // Deep blue (watchdog report, not a crash)
#define COLOR_FG        0xFFD0D0D0  // Light grey
#define COLOR_WHITE     0xFFFFFFFF  // Pure white

#define PANIC_SCALE     2           // Normal panics: large and readable
#define WATCHDOG_SCALE  1           // Watchdog reports are long: native 6x8 glyphs

#define LINE_PADDING    4           // Unscaled pixels between text lines
#define PADDING_START   24

// The panic screen draws straight into VRAM (gfx_display_front) through a
// context of its own. It never touches the back buffer, the console, the
// DWM's contexts, the heap or a lock, so it works whatever state the rest
// of the kernel was in when it went down.
//
// Text position and colors for the iso_* helpers
typedef struct {
    GfxContext* gc;
    int32_t     x;
    int32_t     y;
    int32_t     start_x;
    uint32_t    fg;
    uint32_t    bg;
} TextContext;

volatile bool kernel_panicking = false;

// Forward Declarations
void iso_print(TextContext* ctx, const char* str);
void iso_print_hex32(TextContext* ctx, uint32_t val);

void kernel_crashout(uint32_t error_code, uint32_t faulting_address, uint8_t type, const char* extra) {
    __asm__ __volatile__("cli");
    kernel_panicking = true;

    // A panic before gfx_display_init() (e.g. from pmm_init) has nowhere to
    // draw: halt instead of faulting on a NULL framebuffer and triple-faulting
    if (!gfx_display_ready()) {
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }

    bool watchdog = (type == PT_WATCHDOG);
    uint32_t bg   = watchdog ? COLOR_BG_WD : COLOR_BG;

    // Static: the stack we are on may be nearly exhausted (that may be why
    // we are here), and nothing else can run anymore anyway
    static GfxContext gc;
    gfx_context_init(&gc, gfx_display_front());
    gfx_set_text_scale(&gc, watchdog ? WATCHDOG_SCALE : PANIC_SCALE);

    gfx_clear(&gc, bg);

    TextContext ctx = {
        .gc      = &gc,
        .x       = PADDING_START,
        .y       = PADDING_START,
        .start_x = PADDING_START,
        .fg      = COLOR_FG,
        .bg      = bg
    };

    // Header
    ctx.fg = COLOR_WHITE;
    iso_print(&ctx, watchdog ? "            !!! SCHEDULER WATCHDOG !!!\n"
                             : "            !!! FATAL KERNEL ERROR !!!\n");
    ctx.fg = COLOR_FG;
    iso_print(&ctx, "======================================================\n\n");

    // Process Fault Details
    switch (type) {
        case PT_GENERAL_PROTECTION_FAULT:   iso_print(&ctx, "   GENERAL PROTECTION FAULT\n"); break;
        case PT_PAGE_FAULT:                 iso_print(&ctx, "   PAGE FAULT\n"); break;
        case PT_SEG_FAULT:                  iso_print(&ctx, "   SEGMENTATION FAULT\n"); break;
        case PT_OUT_OF_MEMORY:              iso_print(&ctx, "   OUT-OF-MEMORY\n"); break;
        case PT_CPU_EXCEPTION:              iso_print(&ctx, "   CPU EXCEPTION\n"); break;
        case PT_WATCHDOG:                   iso_print(&ctx, "   SYSTEM STALLED: thread and lock state when the watchdog fired\n"); break;
        default:                            iso_print(&ctx, "   UNKNOWN KERNEL FAULT\n"); break;
    }

    //
    // General & page faults

    if (type == PT_GENERAL_PROTECTION_FAULT) {
        // #GP error code is a segment selector (0 if not segment related), not PF bits
        iso_print(&ctx, "\n\n FAULTING EIP: 0x");
        iso_print_hex32(&ctx, faulting_address);
        iso_print(&ctx, "\n\n SELECTOR ERROR CODE: 0x");
        iso_print_hex32(&ctx, error_code);
        iso_print(&ctx, "\n");

    } else

    if (type == PT_PAGE_FAULT) {
        iso_print(&ctx, "\n\n ACCESS VIOLATION: ");
        iso_print(&ctx, (error_code & PF_WRITE) ? "[WRITE] @ " : "[READ] @ ");

        iso_print(&ctx, "0x");
        iso_print_hex32(&ctx, faulting_address);
        iso_print(&ctx, "\n\n");

        iso_print(&ctx, (error_code & PF_USER) ? " User space\n" : " Kernel space (Supervisor)\n");
        iso_print(&ctx, "\n");
        iso_print(&ctx, (error_code & PF_PRESENT) ? " Page-protection violation\n" : " Page not present violation\n");

    } else

    //
    // Signal segmentation violation

    if (type == PT_SEG_FAULT) {
        iso_print(&ctx, "\n\n ACCESS VIOLATION: ");
        iso_print(&ctx, (error_code & PF_WRITE) ? "[WRITE] @ " : "[READ] @ ");

        iso_print(&ctx, "0x");
        iso_print_hex32(&ctx, faulting_address);
        iso_print(&ctx, "\n\n");

        iso_print(&ctx, (error_code & PF_USER) ? " User space\n" : " Kernel space (Supervisor)\n");
        iso_print(&ctx, "\n");

    } else

    //
    // Out of memory

    if (type == PT_OUT_OF_MEMORY) {
        iso_print(&ctx, "\n\n ALLOCATION FAILURE\n\n");

        if (extra) iso_print(&ctx, extra);

    } else

    //
    // Other CPU exceptions

    if (type == PT_CPU_EXCEPTION) {
        iso_print(&ctx, "\n\n ");
        if (extra) iso_print(&ctx, extra);
        iso_print(&ctx, "\n\n EIP: 0x");
        iso_print_hex32(&ctx, faulting_address);
        iso_print(&ctx, "\n\n ERROR CODE: 0x");
        iso_print_hex32(&ctx, error_code);
        iso_print(&ctx, "\n");

    } else

    //
    // Watchdog report (preformatted by the caller)

    if (type == PT_WATCHDOG) {
        iso_print(&ctx, "\n");
        if (extra) iso_print(&ctx, extra);
    }

    // VRAM is write-combining: make sure every pixel has left the CPU
    __asm__ volatile("sfence" ::: "memory");

    // Halt execution safely
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// Print with '\n' handling. Characters past the right edge are clipped by
// the context; anything past the bottom is not drawn at all.
void iso_print(TextContext* ctx, const char* str) {
    GfxContext* gc = ctx->gc;
    int32_t scale       = gfx_get_text_scale(gc);
    int32_t advance     = gfx_context_char_width(gc);
    int32_t line_height = ((int32_t)gfx_get_font(gc)->glyph_height + LINE_PADDING) * scale;
    int32_t screen_h    = gfx_get_target(gc)->height;

    while (*str) {
        // Off the bottom of the screen: nothing further is visible
        if (ctx->y >= screen_h) return;

        if (*str == '\n') {
            ctx->y += line_height;
            ctx->x = ctx->start_x;
        } else {
            // Unknown characters (>= 0x80 included) show the font's '?'
            gfx_draw_char_opaque(gc, ctx->x, ctx->y, (uint8_t)*str, ctx->fg, ctx->bg);
            ctx->x += advance;
        }
        str++;
    }
}

void iso_print_hex32(TextContext* ctx, uint32_t val) {
    const char hex_chars[] = "0123456789ABCDEF";
    char buffer[9];
    buffer[8] = '\0';

    for (int i = 7; i >= 0; i--) {
        buffer[i] = hex_chars[val & 0xF];
        val >>= 4;
    }

    iso_print(ctx, buffer);
}

