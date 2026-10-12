#include <kernel/dwm/windows/message_box.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/parser.h>

#include <kernel/vfs/vfs.h>
#include <kernel/events.h>

// ==========================================
// Layout (client coordinates, 6x8 font)
// ==========================================
#define MB_FONT_W          6
#define MB_FONT_H          8
#define MB_LINE_H          12     // Font height + 4px leading
#define MB_PAD_X           15     // Left/right text margin
#define MB_PAD_TOP         20     // Client top to first text line
#define MB_TEXT_GAP        16     // Last text line to button
#define MB_PAD_BOTTOM      12     // Button to client bottom
#define MB_BUTTON_W        60
#define MB_BUTTON_H        24
#define MB_TITLEBAR_H      20     // Matches dwm_allocate_window for style 0
#define MB_TITLE_RESERVE   50     // Title text inset + close/minimize buttons

#define MB_MIN_W           200
#define MB_MAX_W           480
#define MB_MESSAGE_MAX     1024

#define MB_KEY_ENTER       0x02
#define MB_KEY_ESCAPE      0x1B

// Word-wrap `src` into `out`, lines separated by '\n'.
//
//  - Breaks at the last space that still fits in max_cols
//  - Honours '\n' already in the message
//  - Hard-breaks single words longer than a line
//  - Stops after max_lines; if text was cut the last line ends in "..."
//
// `out` must hold at least (2 * len + 8) bytes. Returns the line count and
// the longest line (in characters) through out_longest.
static uint16_t mb_wrap(const char* src, size_t len, char* out,
                        uint16_t max_cols, uint16_t max_lines, uint16_t* out_longest) {
    size_t pos = 0;
    size_t o = 0;
    uint16_t lines = 0;
    uint16_t longest = 0;
    
    if (max_cols == 0) max_cols = 1;
    
    while (pos < len && lines < max_lines) {
        size_t remaining = len - pos;
        size_t line_len;
        size_t next;
        
        // Explicit newline within this line (or right at its end)?
        size_t limit = (remaining < (size_t)max_cols + 1) ? remaining : (size_t)max_cols + 1;
        size_t nl = limit;
        for (size_t i = 0; i < limit; i++) {
            if (src[pos + i] == '\n') { nl = i; break; }
        }
        
        if (nl < limit) {
            line_len = nl;
            next = pos + nl + 1;
        } else if (remaining <= max_cols) {
            line_len = remaining;
            next = len;
        } else {
            // Last space at or before column max_cols
            size_t brk = max_cols;
            while (brk > 0 && src[pos + brk] != ' ') brk--;
            
            if (brk > 0) {
                line_len = brk;
                next = pos + brk + 1;
                while (line_len > 0 && src[pos + line_len - 1] == ' ') line_len--;
            } else {
                // One word longer than the line
                line_len = max_cols;
                next = pos + max_cols;
            }
            
            // Wrapped lines don't start with spaces
            while (next < len && src[next] == ' ') next++;
        }
        
        if (lines > 0) out[o++] = '\n';
        memcpy(&out[o], &src[pos], line_len);
        o += line_len;
        
        if (line_len > longest) longest = (uint16_t)line_len;
        lines++;
        pos = next;
    }
    
    // Ran out of lines: mark the cut on the last line
    if (pos < len && lines > 0) {
        size_t line_start = o;
        while (line_start > 0 && out[line_start - 1] != '\n') line_start--;
        
        size_t cur = o - line_start;
        while (cur > 0 && cur + 3 > max_cols) { o--; cur--; }
        
        memcpy(&out[o], "...", 3);
        o += 3;
        cur += 3;
        if (cur > longest) longest = (uint16_t)cur;
    }
    
    out[o] = '\0';
    if (out_longest != NULL) *out_longest = longest;
    return lines;
}

// Longest line (in characters) of an already wrapped string
static uint16_t mb_longest_line(const char* text) {
    uint16_t longest = 0;
    uint16_t cur = 0;
    for (const char* p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            cur = 0;
        } else if (++cur > longest) {
            longest = cur;
        }
    }
    return longest;
}

// Draw a '\n'-separated string, one line per MB_LINE_H
static void mb_draw_lines(const char* text, int16_t x, int16_t y, uint32_t color) {
    char line[MB_MAX_W / MB_FONT_W + 1];
    const char* p = text;
    
    while (*p != '\0') {
        const char* end = p;
        while (*end != '\0' && *end != '\n') end++;
        
        size_t n = (size_t)(end - p);
        if (n > sizeof(line) - 1) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        
        dwm_draw_text(x, y, line, color);
        y += MB_LINE_H;
        
        p = (*end != '\0') ? end + 1 : end;
    }
}

WindowHandle dwm_summon_message_box(const char* title, const char* message) {
    DWM_GUARD();
    if (title == NULL)   title = "";
    if (message == NULL) message = "";
    
    int display_w = display_get_width();
    int display_h = display_get_height();
    
    // Widest box allowed on this display
    int max_w = MB_MAX_W;
    if (max_w > display_w - 40) max_w = display_w - 40;
    if (max_w < MB_MIN_W)       max_w = MB_MIN_W;
    uint16_t max_cols = (uint16_t)((max_w - (MB_PAD_X * 2)) / MB_FONT_W);
    
    // Tallest box that still fits above the taskbar
    int fixed_h = MB_TITLEBAR_H + MB_PAD_TOP + MB_TEXT_GAP + MB_BUTTON_H + MB_PAD_BOTTOM;
    int avail_h = display_h - taskbar.height - 20 - fixed_h;
    uint16_t max_lines = (avail_h > MB_LINE_H) ? (uint16_t)(avail_h / MB_LINE_H) : 1;
    
    // Wrap the message (the window keeps this buffer as its "text" resource)
    size_t message_length = strnlen(message, MB_MESSAGE_MAX);
    char* wrapped = (char*)malloc((message_length * 2) + 8);
    if (wrapped == NULL) return 0;
    
    uint16_t longest = 0;
    uint16_t lines = mb_wrap(message, message_length, wrapped, max_cols, max_lines, &longest);
    if (lines == 0) lines = 1;
    
    // Width: longest line, but never narrower than the title or the minimum
    int width = (longest * MB_FONT_W) + (MB_PAD_X * 2);
    int title_w = (int)strnlen(title, DWM_MAX_TITLE_LEN) * MB_FONT_W + MB_TITLE_RESERVE;
    if (width < title_w)   width = title_w;
    if (width < MB_MIN_W)  width = MB_MIN_W;
    if (width > max_w)     width = max_w;
    
    int height = fixed_h + (lines * MB_LINE_H);
    
    WindowClass wclass_msgbox;
    memset(&wclass_msgbox, 0, sizeof(WindowClass));
    
    // Center in the area above the taskbar
    int x = (display_w - width) / 2;
    int y = (display_h - taskbar.height - height) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    
    wclass_msgbox.width      = (uint16_t)width;
    wclass_msgbox.height     = (uint16_t)height;
    wclass_msgbox.x          = (uint16_t)x;
    wclass_msgbox.y          = (uint16_t)y;
    wclass_msgbox.max_width  = (uint16_t)width;
    wclass_msgbox.max_height = (uint16_t)height;
    
    strncpy(wclass_msgbox.title, title, DWM_MAX_TITLE_LEN - 1);
    wclass_msgbox.title[DWM_MAX_TITLE_LEN - 1] = '\0';
    
    struct WindowObject* msg_handle = dwm_allocate_window(
        wclass_msgbox,
        0,
        (WindowProcedure)callback_message_box_handler
    );
    
    if (msg_handle == NULL) {
        free(wrapped);
        return 0;
    }
    
    if (!dwm_window_resource_add(msg_handle->id, "text", wrapped)) {
        free(wrapped);
    }
    
    dwm_set_focus(msg_handle);
    return msg_handle->id;
}

void callback_message_box_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return;
    
    // window->h includes the titlebar; the client area starts below it
    uint16_t client_h = window->h - window->titlebar_height;
    uint16_t button_x = (window->w - MB_BUTTON_W) / 2;
    uint16_t button_y = client_h - MB_PAD_BOTTOM - MB_BUTTON_H;
    
    switch (event) {
        case DWM_EVENT_REDRAW: {
            // Window background
            dwm_draw_rect_filled(0, 0, window->w, window->h, theme.client.background);
            
            // Message text: lines left aligned, the block centered
            char* message_resource = (char*)dwm_window_resource_get_by_name(handle, "text");
            if (message_resource != NULL) {
                int block_w = mb_longest_line(message_resource) * MB_FONT_W;
                int text_x = (window->w - block_w) / 2;
                if (text_x < MB_PAD_X) text_x = MB_PAD_X;
                
                mb_draw_lines(message_resource, (int16_t)text_x, MB_PAD_TOP, theme.client.text);
            }
            
            // Button body
            dwm_draw_rect_filled(button_x, button_y, MB_BUTTON_W, MB_BUTTON_H, theme.button.fill);
            dwm_draw_rect(button_x, button_y, MB_BUTTON_W, MB_BUTTON_H, theme.button.border);
            
            // Button label, centered
            const char* button_text = "ok";
            uint16_t text_width = strlen(button_text) * MB_FONT_W;
            uint16_t label_x = button_x + ((MB_BUTTON_W - text_width) / 2);
            uint16_t label_y = button_y + ((MB_BUTTON_H - MB_FONT_H) / 2);
            dwm_draw_text(label_x, label_y, button_text, theme.button.text);
            break;
        }
        
        case DWM_EVENT_MOUSE: {
            if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) break;
            
            uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
            uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);
            
            if (click_x >= button_x && click_x < (button_x + MB_BUTTON_W) &&
                click_y >= button_y && click_y < (button_y + MB_BUTTON_H)) {
                dwm_window_send_event(handle, DWM_EVENT_CLOSE);
            }
            break;
        }
        
        case DWM_EVENT_KEYBOARD: {
            uint8_t key = (uint8_t)(wparam & 0xFF);
            if (key == MB_KEY_ESCAPE || key == MB_KEY_ENTER) {
                dwm_window_send_event(handle, DWM_EVENT_CLOSE);
            }
            break;
        }
        
        default:
            break;
    }
}
