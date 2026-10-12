#include <kernel/panic/panic_error.h>
#include <stdint.h>
#include <stdbool.h>

// Layout & Styling Constants
#define COLOR_BG        0xFF3A0000  // Deep dark red (crash)
#define COLOR_BG_WD     0xFF00203A  // Deep blue (watchdog report, not a crash)
#define COLOR_FG        0xFFD0D0D0  // Light grey
#define COLOR_WHITE     0xFFFFFFFF  // Pure white

#define PANIC_SCALE     2           // Normal panics: large and readable
#define WATCHDOG_SCALE  1           // Watchdog reports are long: native 6x8 glyphs

#define GLYPH_WIDTH     6
#define GLYPH_HEIGHT    8
#define GLYPH_COUNT     128         // Glyphs in char_rom
#define PADDING_START   24

extern struct MultibootInfo* vinfo;
extern uint32_t* front_buffer;
extern const uint8_t char_rom[];

// Simplifies passing text rendering configurations
typedef struct {
    int x;
    int y;
    int start_x;
    uint32_t fg;
    uint32_t bg;
} TextContext;

volatile bool kernel_panicking = false;

// Pixel scale used by every iso_* call (set once per panic)
static int text_scale = PANIC_SCALE;

// Forward Declarations
void iso_draw_char(char ch, int x, int y, uint32_t fg_color, uint32_t bg_color);
void iso_print(TextContext* ctx, const char* str);
void iso_print_hex32(TextContext* ctx, uint32_t val);

static void clear_screen(uint32_t color) {
    uint32_t screen_w = vinfo->framebuffer_width;
    uint32_t screen_h = vinfo->framebuffer_height;
    uint32_t stride   = vinfo->framebuffer_pitch / 4;
    
    for (uint32_t y = 0; y < screen_h; y++) {
        for (uint32_t x = 0; x < screen_w; x++) {
            front_buffer[y * stride + x] = color;
        }
    }
}

void kernel_crashout(uint32_t error_code, uint32_t faulting_address, uint8_t type, const char* extra) {
    __asm__ __volatile__("cli");
    kernel_panicking = true;
    
    // A panic before draw_set_info()/draw_set_frame_front_buffer() (e.g. from
    // pmm_init) would otherwise fault on a NULL pointer and triple-fault.
    if (vinfo == NULL || front_buffer == NULL) {
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }
    
    bool watchdog = (type == PT_WATCHDOG);
    uint32_t bg   = watchdog ? COLOR_BG_WD : COLOR_BG;
    text_scale    = watchdog ? WATCHDOG_SCALE : PANIC_SCALE;
    
    clear_screen(bg);
    
    TextContext ctx = {
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
    
    // Force write memory fence ensuring pixels have hit VRAM
    __asm__ volatile("sfence" ::: "memory");
    
    // Halt execution safely
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void iso_draw_char(char ch, int x, int y, uint32_t fg_color, uint32_t bg_color) {
    uint32_t stride   = vinfo->framebuffer_pitch / 4;
    uint32_t screen_w = vinfo->framebuffer_width;
    uint32_t screen_h = vinfo->framebuffer_height;
    
    // char is signed: anything >= 0x80 would index far past the font
    uint8_t glyph = (uint8_t)ch;
    if (glyph >= GLYPH_COUNT) glyph = '?';
    uint32_t rom_index = (uint32_t)glyph * GLYPH_WIDTH;
    
    for (int col = 0; col < GLYPH_WIDTH; col++) {
        uint8_t column_mask = char_rom[rom_index + col];
        
        for (int row = 0; row < GLYPH_HEIGHT; row++) {
            uint32_t color = (column_mask & (1 << row)) ? fg_color : bg_color;
            
            // Map a single font bit into a larger block of screen pixels
            for (int sy = 0; sy < text_scale; sy++) {
                int pixel_y = y + (row * text_scale) + sy;
                if (pixel_y < 0 || pixel_y >= (int)screen_h) continue;
                
                for (int sx = 0; sx < text_scale; sx++) {
                    int pixel_x = x + (col * text_scale) + sx;
                    if (pixel_x < 0 || pixel_x >= (int)screen_w) continue;
                    
                    front_buffer[pixel_y * stride + pixel_x] = color;
                }
            }
        }
    }
}

void iso_print(TextContext* ctx, const char* str) {
    int line_height = (GLYPH_HEIGHT + 4) * text_scale;   // Font height + padding
    int screen_w    = (int)vinfo->framebuffer_width;
    int screen_h    = (int)vinfo->framebuffer_height;
    
    while (*str) {
        // Off the bottom of the screen: nothing further is visible
        if (ctx->y >= screen_h) return;
        
        if (*str == '\n') {
            ctx->y += line_height;
            ctx->x = ctx->start_x;
        } else {
            if (ctx->x < screen_w)
                iso_draw_char(*str, ctx->x, ctx->y, ctx->fg, ctx->bg);
            ctx->x += GLYPH_WIDTH * text_scale;
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

