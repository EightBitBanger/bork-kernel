#include <stdio.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/dwm/dwm.h>
#include <kernel/events.h>
#include <kernel/util/string.h>
#include <kernel/malloc.h>
#include <kernel/console/keyboard.h>

#include <kernel/programs/notepad/internal.h>

// Matching definitions from the kernel driver mappings
#define KBD_CUSTOM_BACKSPACE    0x01
#define KBD_CUSTOM_ENTER        0x02

// Native PS/2 Scan Code Set 1
#define KBD_SCANCODE_DELETE     0x53
#define KBD_SCANCODE_HOME       0x47
#define KBD_SCANCODE_END        0x4F
#define KBD_SCANCODE_UP         0x48
#define KBD_SCANCODE_LEFT       0x4B
#define KBD_SCANCODE_RIGHT      0x4D
#define KBD_SCANCODE_DOWN       0x50

#define KBD_SCANCODE_LSHIFT     0x2A
#define KBD_SCANCODE_RSHIFT     0x36
#define KBD_SCANCODE_CTRL       0x1D   // Right ctrl is E0 1D, same low byte

#define KBD_SCANCODE_A          0x1E
#define KBD_SCANCODE_C          0x2E
#define KBD_SCANCODE_V          0x2F
#define KBD_SCANCODE_X          0x2D

// Menu Bar Layout Geometry
#define MENUBAR_HEIGHT          16
#define MENUBAR_TEXT_X          10
#define MENUBAR_TEXT_Y          4
#define MENUBAR_CLICK_WIDTH     36

// Selection highlight
#define SELECTION_COLOR         (dwm_get_theme()->edit.selection)
#define CARET_COLOR             (dwm_get_theme()->client.text_value)

// The renderer builds each visual line in a fixed scratch buffer, so a line
// can never be wider than this. Every layout function uses the same limit.
#define LINE_SCRATCH_SIZE       128

// Context menus
static const char* file_menu_options[]   = { "New", "Open", "Save", "Clear", "Exit" };
static const char* canvas_menu_options[] = { "Cut", "Copy", "Paste", "Delete", "Select all" };

#define FILE_MENU_COUNT    (sizeof(file_menu_options)   / sizeof(file_menu_options[0]))
#define CANVAS_MENU_COUNT  (sizeof(canvas_menu_options) / sizeof(canvas_menu_options[0]))

// ==========================================
// Modifier keys
// ==========================================
//
// Shift / Ctrl come from the keyboard driver. Modifier keys produce no
// character, so they never arrive as DWM_EVENT_KEYBOARD messages and can't
// be tracked from the key stream here.

static bool notepad_shift_down(void) { return kb_shift_down(); }
static bool notepad_ctrl_down(void)  { return kb_ctrl_down();  }

// ==========================================
// State lookup
// ==========================================

static struct NotepadWindowState* get_notepad_window_state(WindowHandle handle) {
    struct NotepadWindowState* current = notepad_window_list_head;
    while (current != NULL) {
        if (current->handle == handle) return current;
        current = current->next;
    }
    return NULL;
}

static void handle_notepad_resize(struct NotepadWindowState* state, uint32_t wparam) {
    state->win_width = (uint16_t)(wparam & 0xFFFF);
    state->win_height = (uint16_t)((wparam >> 16) & 0xFFFF);
}

// ==========================================
// Layout
// ==========================================
//
// Rules shared by drawing, the arrow keys and mouse hit testing:
//   - byte i sits at (row, col); the caret "at i" is drawn left of byte i
//   - '\n' ends the row
//   - any other byte advances col; reaching max_chars wraps to the next row
// The buffer is binary: '\0' is an ordinary byte and is drawn as a space.

static uint16_t notepad_max_chars(WindowHandle handle) {
    uint16_t window_width = dwm_window_get_width(handle);
    int chars = ((int)window_width - (TEXT_PADDING_X * 2)) / TEXT_FONT_CHAR_WIDTH;
    if (chars < 1) chars = 1;
    if (chars > LINE_SCRATCH_SIZE - 1) chars = LINE_SCRATCH_SIZE - 1;
    return (uint16_t)chars;
}

static void layout_pos_of(struct NotepadWindowState* state, uint16_t max_chars, uint32_t target_idx,
                          uint16_t* out_row, uint16_t* out_col) {
    uint16_t r = 0, c = 0;
    for (uint32_t i = 0; i < target_idx && i < state->text_length; i++) {
        if (state->text_buffer[i] == '\n') {
            r++;
            c = 0;
        } else if (++c >= max_chars) {
            r++;
            c = 0;
        }
    }
    *out_row = r;
    *out_col = c;
}

// Caret index nearest to (row, col). Past the end of a line snaps to the end
// of that line, past the last row snaps to the end of the text.
static uint32_t layout_index_at(struct NotepadWindowState* state, uint16_t max_chars,
                                uint16_t target_row, uint16_t target_col) {
    uint16_t r = 0, c = 0;
    for (uint32_t i = 0; i < state->text_length; i++) {
        if (r == target_row && c >= target_col) return i;

        if (state->text_buffer[i] == '\n') {
            if (r == target_row) return i;   // Before the line break
            r++;
            c = 0;
        } else if (++c >= max_chars) {
            r++;
            c = 0;
        }
    }
    return state->text_length;
}

// Window-local point (signed: may be outside the window while dragging)
static uint32_t layout_index_from_point(WindowHandle handle, struct NotepadWindowState* state,
                                        int x, int y) {
    uint16_t max_chars = notepad_max_chars(handle);
    int text_top = TEXT_PADDING_Y + MENUBAR_HEIGHT;

    if (y < text_top) y = text_top;
    if (x < TEXT_PADDING_X) x = TEXT_PADDING_X;

    // Round to the nearest gap between characters, not the cell under the pointer
    int col = (x - TEXT_PADDING_X + (TEXT_FONT_CHAR_WIDTH / 2)) / TEXT_FONT_CHAR_WIDTH;
    int row = (y - text_top) / TEXT_FONT_LINE_HEIGHT;

    if (col > max_chars - 1) col = max_chars - 1;
    if (row > 0xFFFF) row = 0xFFFF;

    return layout_index_at(state, max_chars, (uint16_t)row, (uint16_t)col);
}

// ==========================================
// Selection & editing primitives
// ==========================================

static bool has_selection(struct NotepadWindowState* state) {
    return state->sel_anchor != state->cursor_index;
}

static uint32_t selection_start(struct NotepadWindowState* state) {
    return (state->sel_anchor < state->cursor_index) ? state->sel_anchor : state->cursor_index;
}

static uint32_t selection_end(struct NotepadWindowState* state) {
    return (state->sel_anchor > state->cursor_index) ? state->sel_anchor : state->cursor_index;
}

// Move the caret. extend = keep the anchor where it is (grow the selection)
static void move_cursor(struct NotepadWindowState* state, uint32_t index, bool extend) {
    if (index > state->text_length) index = state->text_length;
    state->cursor_index = index;
    if (!extend) state->sel_anchor = index;
}

static void select_range(struct NotepadWindowState* state, uint32_t start, uint32_t end) {
    state->sel_anchor   = start;
    state->cursor_index = end;
}

// Make room for `extra` more bytes. The buffer is binary, so no terminator.
static bool ensure_capacity(struct NotepadWindowState* state, uint32_t extra) {
    uint32_t needed = state->text_length + extra;
    if (state->text_buffer != NULL && needed <= state->text_capacity) return true;

    if (needed < state->text_length) return false;      // Wrapped around

    // Double for ordinary typing, but a big paste gets exactly what it needs
    // (plus a little room): doubling a multi-megabyte buffer to fit one more
    // megabyte would ask the heap for far more than the text takes
    uint32_t new_capacity = (state->text_capacity > 0) ? state->text_capacity : 2048;
    if (new_capacity <= 0x7FFFFFFFu) new_capacity *= 2;
    if (new_capacity < needed) {
        new_capacity = (needed <= 0xFFFFFFFFu - 4096u) ? needed + 4096u : needed;
    }

    char* new_buf = (char*)realloc(state->text_buffer, new_capacity);
    if (new_buf == NULL) return false;

    state->text_buffer = new_buf;
    state->text_capacity = new_capacity;
    return true;
}

static void delete_range(struct NotepadWindowState* state, uint32_t start, uint32_t end) {
    if (end > state->text_length) end = state->text_length;
    if (start >= end) return;

    memmove(&state->text_buffer[start], &state->text_buffer[end], state->text_length - end);
    state->text_length -= (end - start);
    state->modified = true;
    move_cursor(state, start, false);
}

static bool delete_selection(struct NotepadWindowState* state) {
    if (!has_selection(state)) return false;
    delete_range(state, selection_start(state), selection_end(state));
    return true;
}

// Insert raw bytes at the caret, replacing the selection
static bool insert_bytes(struct NotepadWindowState* state, const char* data, uint32_t length) {
    delete_selection(state);
    if (length == 0) return true;
    if (!ensure_capacity(state, length)) return false;

    uint32_t at = state->cursor_index;
    memmove(&state->text_buffer[at + length], &state->text_buffer[at], state->text_length - at);
    memcpy(&state->text_buffer[at], data, length);
    state->text_length += length;
    state->modified = true;
    move_cursor(state, at + length, false);
    return true;
}

// ==========================================
// Clipboard commands
// ==========================================

// Copies plain text as TEXT, so explorer/desktop Paste and other programs
// see it as text. A selection containing '\0' bytes (the buffer is binary)
// goes up as raw BINARY instead, so nothing is lost.
static bool notepad_copy(struct NotepadWindowState* state) {
    if (!has_selection(state)) return false;
    
    uint32_t start  = selection_start(state);
    uint32_t length = selection_end(state) - start;
    const char* data = &state->text_buffer[start];
    
    bool has_nul = false;
    for (uint32_t i = 0; i < length; i++) {
        if (data[i] == '\0') { has_nul = true; break; }
    }
    
    bool ok = has_nul ? dwm_clipboard_set(data, length)
                      : dwm_clipboard_set_text_n(data, length);
    if (!ok) {
        dwm_notify("Copy", "The selection is too large to copy", DWM_NOTIFY_LONG_MS);
    }
    return ok;
}

static void notepad_cut(struct NotepadWindowState* state) {
    // Only delete what actually made it onto the clipboard
    if (notepad_copy(state)) {
        delete_selection(state);
    }
}

// Pastes straight out of the clipboard's own storage (a view), so a
// multi-megabyte paste needs no temporary copy: only the document grows.
// TEXT and BINARY bytes go in as they are; FILES paste as their paths, one
// per line (the '\0' separators become '\n' after inserting).
static void notepad_paste(struct NotepadWindowState* state) {
    DWMClipboardView view;
    if (!dwm_clipboard_view_acquire(&view)) return;     // Empty

    const char* data = (const char*)view.data;
    uint32_t size = 0;
    bool files = false;

    if (view.format == DWM_CLIPBOARD_TEXT || view.format == DWM_CLIPBOARD_BINARY) {
        size = view.size;
    } else if (view.format == DWM_CLIPBOARD_FILES) {
        size = (view.size > 0) ? view.size - 1 : 0;     // Drop the final '\0'
        files = true;
    }

    if (size == 0) {                                    // An image, or nothing
        dwm_clipboard_view_release(&view);
        return;
    }

    if (!insert_bytes(state, data, size)) {
        dwm_notify("Paste", "Not enough memory to paste", DWM_NOTIFY_LONG_MS);
    } else if (files) {
        uint32_t start = state->cursor_index - size;
        for (uint32_t i = start; i < state->cursor_index; i++) {
            if (state->text_buffer[i] == '\0') state->text_buffer[i] = '\n';
        }
    }

    dwm_clipboard_view_release(&view);
}

static void notepad_select_all(struct NotepadWindowState* state) {
    select_range(state, 0, state->text_length);
}

// ==========================================
// Word selection (double click)
// ==========================================

// 0 = word, 1 = blank, 2 = line break, 3 = anything else
static int char_class(char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') return 0;
    if (ch == ' ' || ch == '\t' || ch == '\0') return 1;
    if (ch == '\n') return 2;
    return 3;
}

static void select_word_at(struct NotepadWindowState* state, uint32_t index) {
    if (state->text_length == 0) return;

    // A double click right after the last char targets that char
    if (index >= state->text_length) index = state->text_length - 1;

    int cls = char_class(state->text_buffer[index]);
    if (cls == 2) {
        select_range(state, index, index + 1);
        return;
    }

    uint32_t start = index;
    uint32_t end = index + 1;
    while (start > 0 && char_class(state->text_buffer[start - 1]) == cls) start--;
    while (end < state->text_length && char_class(state->text_buffer[end]) == cls) end++;

    select_range(state, start, end);
}

// ==========================================
// Keyboard
// ==========================================

static void handle_notepad_keypress(WindowHandle handle, struct NotepadWindowState* state, uint32_t wparam) {
    char ascii_char  = (char)(wparam & 0xFF);
    uint8_t scancode = (uint8_t)((wparam >> 8) & 0xFF);

    bool released = (scancode & 0x80) || (wparam & 0x80000000);
    uint8_t key = scancode & 0x7F;

    if (released) return;

    bool shift = notepad_shift_down();
    bool ctrl  = notepad_ctrl_down();

    bool is_navigation = (key == KBD_SCANCODE_LEFT || key == KBD_SCANCODE_RIGHT ||
                          key == KBD_SCANCODE_UP   || key == KBD_SCANCODE_DOWN  ||
                          key == KBD_SCANCODE_HOME || key == KBD_SCANCODE_END);

    // ---- Ctrl shortcuts (matched by scancode, or by letter as a fallback) ----
    if (ctrl && !is_navigation) {
        char letter = ascii_char | 0x20;   // Lower case

        if (key == KBD_SCANCODE_A || letter == 'a') {
            notepad_select_all(state);
        } else if (key == KBD_SCANCODE_C || letter == 'c') {
            notepad_copy(state);
            return;                          // Nothing changed on screen
        } else if (key == KBD_SCANCODE_X || letter == 'x') {
            notepad_cut(state);
        } else if (key == KBD_SCANCODE_V || letter == 'v') {
            notepad_paste(state);
        } else {
            return;                          // Swallow other Ctrl+key combos
        }

        state->has_preferred_col = false;
        dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        return;
    }

    // ---- Navigation (Shift extends the selection) ----
    if (is_navigation) {
        uint16_t max_chars = notepad_max_chars(handle);
        uint16_t cur_row = 0, cur_col = 0;
        layout_pos_of(state, max_chars, state->cursor_index, &cur_row, &cur_col);

        if (key != KBD_SCANCODE_UP && key != KBD_SCANCODE_DOWN) {
            state->has_preferred_col = false;
        }

        switch (key) {
            case KBD_SCANCODE_LEFT:
                if (!shift && has_selection(state)) {
                    move_cursor(state, selection_start(state), false);   // Collapse to the left edge
                } else if (state->cursor_index > 0) {
                    move_cursor(state, state->cursor_index - 1, shift);
                }
                break;

            case KBD_SCANCODE_RIGHT:
                if (!shift && has_selection(state)) {
                    move_cursor(state, selection_end(state), false);
                } else if (state->cursor_index < state->text_length) {
                    move_cursor(state, state->cursor_index + 1, shift);
                }
                break;

            case KBD_SCANCODE_UP:
            case KBD_SCANCODE_DOWN:
                if (!state->has_preferred_col) {
                    state->preferred_col = cur_col;
                    state->has_preferred_col = true;
                }

                if (key == KBD_SCANCODE_UP) {
                    if (cur_row == 0) {
                        move_cursor(state, 0, shift);
                    } else {
                        move_cursor(state, layout_index_at(state, max_chars, cur_row - 1, state->preferred_col), shift);
                    }
                } else {
                    move_cursor(state, layout_index_at(state, max_chars, cur_row + 1, state->preferred_col), shift);
                }
                break;

            case KBD_SCANCODE_HOME:   // Ctrl+Home = start of the document
                if (ctrl) move_cursor(state, 0, shift);
                else      move_cursor(state, layout_index_at(state, max_chars, cur_row, 0), shift);
                break;

            case KBD_SCANCODE_END:    // Ctrl+End = end of the document
                if (ctrl) move_cursor(state, state->text_length, shift);
                else      move_cursor(state, layout_index_at(state, max_chars, cur_row, max_chars), shift);
                break;
        }

        dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        return;
    }

    state->has_preferred_col = false;

    // ---- Delete (character after the caret, or the selection) ----
    if (key == KBD_SCANCODE_DELETE) {
        if (!delete_selection(state) && state->cursor_index < state->text_length) {
            delete_range(state, state->cursor_index, state->cursor_index + 1);
        }
        dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        return;
    }

    // ---- Backspace (character before the caret, or the selection) ----
    if (ascii_char == KBD_CUSTOM_BACKSPACE) {
        if (!delete_selection(state) && state->cursor_index > 0) {
            delete_range(state, state->cursor_index - 1, state->cursor_index);
        }
        dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        return;
    }

    // Any other special key with no character
    if (ascii_char == 0) return;

    // ---- Typed character (replaces the selection) ----
    if (ascii_char == KBD_CUSTOM_ENTER) ascii_char = '\n';

    insert_bytes(state, &ascii_char, 1);
    dwm_window_send_event(handle, DWM_EVENT_REDRAW);
}

// ==========================================
// Mouse
// ==========================================

static void handle_notepad_mouse(WindowHandle handle, struct NotepadWindowState* state, uint32_t wparam, int32_t lparam) {
    uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
    uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);

    // Right-click context menu (keeps the current selection for Cut / Copy)
    if (lparam & DWM_STATE_MOUSE_BTN_RIGHT) {
        context_directive = CONTEXT_DIRECTIVE_CANVAS;
        dwm_summon_context_menu(handle, click_x, click_y, canvas_menu_options, CANVAS_MENU_COUNT);
        return;
    }

    if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) return;

    // Titlebar / border clicks arrive with a negative y (above the surface)
    if ((int16_t)click_y < 0 || (int16_t)click_x < 0) return;

    // "File" menu button
    if (click_x >= MENUBAR_TEXT_X && click_x < (MENUBAR_TEXT_X + MENUBAR_CLICK_WIDTH) &&
        click_y < MENUBAR_HEIGHT) {
        context_directive = CONTEXT_DIRECTIVE_FILE;
        dwm_summon_context_menu(handle, MENUBAR_TEXT_X, MENUBAR_HEIGHT, file_menu_options, FILE_MENU_COUNT);
        return;
    }

    // Anything else on the menubar row does nothing (and doesn't start a drag)
    if (click_y < MENUBAR_HEIGHT) return;

    uint32_t index = layout_index_from_point(handle, state, click_x, click_y);
    state->has_preferred_col = false;

    if (lparam & DWM_STATE_MOUSE_DOUBLE_CLK) {
        select_word_at(state, index);
        state->mouse_selecting = false;
    } else {
        // Shift+click extends the existing selection
        move_cursor(state, index, notepad_shift_down());
        state->mouse_selecting = true;
    }

    dwm_window_send_event(handle, DWM_EVENT_REDRAW);
}

static void handle_notepad_mouse_move(WindowHandle handle, struct NotepadWindowState* state, uint32_t wparam, int32_t lparam) {
    if (!state->mouse_selecting) return;
    if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) {
        state->mouse_selecting = false;
        return;
    }

    // Signed: the pointer may be left of / above the window while dragging
    int x = (int16_t)(wparam & 0xFFFF);
    int y = (int16_t)((wparam >> 16) & 0xFFFF);

    uint32_t index = layout_index_from_point(handle, state, x, y);
    if (index != state->cursor_index) {
        move_cursor(state, index, true);
        dwm_window_send_event(handle, DWM_EVENT_REDRAW);
    }
}

// ==========================================
// Drawing
// ==========================================

static void handle_notepad_redraw(WindowHandle handle, struct NotepadWindowState* state) {
    const struct DWMTheme* t = dwm_get_theme();

    uint16_t window_width = dwm_window_get_width(handle);
    uint16_t window_height = dwm_window_get_height(handle);

    dwm_draw_rect_filled(NOTEPAD_BG_X, NOTEPAD_BG_Y, window_width, window_height, t->client.background);

    dwm_draw_rect_filled(0, 0, window_width, MENUBAR_HEIGHT, t->button.fill);
    dwm_draw_line(0, MENUBAR_HEIGHT - 1, window_width, 0, t->button.border);
    dwm_draw_text(MENUBAR_TEXT_X, MENUBAR_TEXT_Y, "File", t->client.text);

    if (state->text_buffer == NULL) return;

    uint16_t max_chars = notepad_max_chars(handle);
    int16_t start_y = TEXT_PADDING_Y + MENUBAR_HEIGHT;

    uint32_t sel_start = selection_start(state);
    uint32_t sel_end   = selection_end(state);

    char line_scratchpad[LINE_SCRATCH_SIZE];
    uint16_t scratch_idx = 0;

    uint16_t row = 0;
    uint16_t col = 0;

    // Pending selection run on the current row (merged into one rect)
    bool     hl_active = false;
    uint16_t hl_col = 0;
    uint16_t hl_len = 0;

    bool     cursor_visible = false;
    int16_t  cursor_x = 0;
    int16_t  cursor_y = 0;

    for (uint32_t i = 0; i <= state->text_length; i++) {
        int16_t row_y = start_y + (row * TEXT_FONT_LINE_HEIGHT);

        // Everything from here on is below the window
        if (row_y > window_height) break;

        if (i == state->cursor_index) {
            cursor_visible = true;
            cursor_x = TEXT_PADDING_X + (col * TEXT_FONT_CHAR_WIDTH);
            cursor_y = row_y;
        }

        if (i == state->text_length) break;

        char target = state->text_buffer[i];

        // Selection: extend the run or start a new one. A selected line break
        // gets one cell so selected empty lines are still visible.
        if (i >= sel_start && i < sel_end) {
            if (!hl_active) {
                hl_active = true;
                hl_col = col;
                hl_len = 0;
            }
            hl_len++;
        }

        bool end_of_row = false;

        if (target == '\n') {
            end_of_row = true;
        } else {
            if (target == '\0') {
                line_scratchpad[scratch_idx++] = ' ';
            } else if (target < 32 || target > 126) {
                line_scratchpad[scratch_idx++] = '.';   // Control / non-ASCII byte
            } else {
                line_scratchpad[scratch_idx++] = target;
            }
            col++;
            if (col >= max_chars) end_of_row = true;
        }

        bool selection_ends = hl_active && (i + 1 >= sel_end);

        // Highlight goes down before the row's text so the glyphs land on top
        if (hl_active && (end_of_row || selection_ends)) {
            dwm_draw_rect_filled(TEXT_PADDING_X + (hl_col * TEXT_FONT_CHAR_WIDTH), row_y - 2,
                                 hl_len * TEXT_FONT_CHAR_WIDTH, TEXT_FONT_LINE_HEIGHT, SELECTION_COLOR);
            hl_active = false;
        }

        if (end_of_row || i == state->text_length - 1) {
            if (scratch_idx > 0) {
                line_scratchpad[scratch_idx] = '\0';
                dwm_draw_text(TEXT_PADDING_X, row_y, line_scratchpad, t->client.text_soft);
            }
            scratch_idx = 0;

            if (end_of_row) {
                row++;
                col = 0;
            }
        }
    }

    if (cursor_visible) {
        dwm_draw_line(cursor_x, cursor_y - 1, 0, TEXT_FONT_LINE_HEIGHT - 2, CARET_COLOR);
    }
}

// ==========================================
// File
// ==========================================

// Returns true when the document is on disk; on failure the user has
// already been told why
static bool notepad_save(struct NotepadWindowState* state) {
    uint8_t permissions = 0;
    vfs_get_permissions(state->file_path, &permissions);

    if (!(permissions & VFS_PERMISSION_WRITE)) {
        dwm_notify("Notepad", "Cannot save: access denied", DWM_NOTIFY_LONG_MS);
        return false;
    }

    // Size the file to exactly the buffer first: writes don't reliably extend
    // a file, and without this a shorter document left stale bytes behind
    if (!vfs_truncate(state->file_path, state->text_length)) {
        dwm_notify("Notepad", "Unable to save file", DWM_NOTIFY_LONG_MS);
        return false;
    }

    if (state->text_length > 0) {
        File file = vfs_open(state->file_path, VFS_OPEN_WRITE);
        if (file == VFS_INVALID_FILE) {
            dwm_notify("Notepad", "Unable to save file", DWM_NOTIFY_LONG_MS);
            return false;
        }

        vfs_seek(file, 0);
        int32_t written = vfs_write(file, state->text_buffer, state->text_length);
        vfs_close(file);

        if (written != (int32_t)state->text_length) {
            dwm_notify("Notepad", "Unable to save file", DWM_NOTIFY_LONG_MS);
            return false;
        }
    }

    state->modified = false;

    // Save has no other feedback, so say it worked
    char saved[80];
    saved[0] = '\0';
    strlcat(saved, "Saved ", sizeof(saved));
    strlcat(saved, (state->file_path[0] != '\0') ? state->window_title : "Untitled", sizeof(saved));
    dwm_notify("Notepad", saved, 3000);
    return true;
}

// ==========================================
// Exit confirmation
// ==========================================

// Answer from the "save changes?" dialog (DWM thread, DWM lock held)
static void notepad_confirm_exit(DWMConfirmResult result, void* user) {
    WindowHandle handle = (WindowHandle)(uintptr_t)user;
    struct NotepadWindowState* state = get_notepad_window_state(handle);
    if (state == NULL) return;              // Window already gone

    state->confirm_window = 0;

    if (result == DWM_CONFIRM_CANCEL) return;

    // A failed save has shown its own error: stay open so nothing is lost
    if (result == DWM_CONFIRM_YES && !notepad_save(state)) return;

    state->close_confirmed = true;
    dwm_window_send_event(handle, DWM_EVENT_CLOSE);
}

static void notepad_ask_save(WindowHandle handle, struct NotepadWindowState* state) {
    // Already asking: bring the question back to the front
    if (state->confirm_window != 0) {
        dwm_window_set_focus(state->confirm_window);
        return;
    }

    const char* name = (state->file_path[0] != '\0') ? state->window_title : "Untitled";
    // "Save changes to <name>?" (a long name is cut, the '?' always fits)
    char message[80];
    message[0] = '\0';
    strlcat(message, "Save changes to ", sizeof(message));
    strlcat(message, name, sizeof(message) - 1);
    strlcat(message, "?", sizeof(message));

    WindowHandle confirm = dwm_summon_message_confirm("Notepad", message, "Save", "Don't save",
                                                      notepad_confirm_exit, (void*)(uintptr_t)handle);
    if (confirm == 0) {
        // Couldn't ask: keep the document rather than drop it
        dwm_notify("Notepad", "Save your changes before closing", DWM_NOTIFY_LONG_MS);
        return;
    }
    state->confirm_window = confirm;
}

void dialog_callback(const char* path, bool cancelled) {

}

// ==========================================
// Window procedure
// ==========================================

void callback_handler_notepad(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct NotepadWindowState* state = get_notepad_window_state(handle);
    if (!state) return;

    // Every edit path assumes a buffer exists (the load path can leave it NULL)
    if (state->text_buffer == NULL && event != DWM_EVENT_DESTROY) {
        state->text_capacity = 0;
        state->text_length = 0;
        if (!ensure_capacity(state, 1)) return;
    }

    switch (event) {
    case DWM_EVENT_MOUSE:
        handle_notepad_mouse(handle, state, wparam, lparam);
        break;

    case DWM_EVENT_MOUSE_MOVE:
        handle_notepad_mouse_move(handle, state, wparam, lparam);
        break;

    case DWM_EVENT_MOUSE_UP:
        state->mouse_selecting = false;
        break;

    case DWM_EVENT_KEYBOARD:
        handle_notepad_keypress(handle, state, wparam);
        break;

    // Drop any drag-select in progress when focus changes
    case DWM_EVENT_FOCUS_GAINED:
    case DWM_EVENT_FOCUS_LOST:
        state->mouse_selecting = false;
        break;

    case DWM_EVENT_RESIZE:
        handle_notepad_resize(state, wparam);
        break;

    case DWM_EVENT_REDRAW:
        handle_notepad_redraw(handle, state);
        break;

    // Close box or File > Exit: ask first when there are unsaved changes
    case DWM_EVENT_CLOSE:
        if (state->modified && !state->close_confirmed) {
            dwm_window_cancel_close(handle);
            notepad_ask_save(handle, state);
        }
        return;

    case DWM_EVENT_DESTROY: {
        // A prompt still open for this window answers Cancel to nobody
        WindowHandle confirm = state->confirm_window;
        free_notepad_window_state(handle);
        if (confirm != 0) dwm_window_send_event(confirm, DWM_EVENT_CLOSE);
        return;
    }

    case DWM_EVENT_CONTEXT_MENU:

        if (context_directive == CONTEXT_DIRECTIVE_FILE) {
            switch (wparam) {

            case 0: // New
                dwm_notify("Notepad", "New is not implemented yet", DWM_NOTIFY_DEFAULT_MS);
                break;

            case 1: // Open
                dwm_notify("Notepad", "Open is not implemented yet", DWM_NOTIFY_DEFAULT_MS);
                break;

            case 2: // Save
                notepad_save(state);
                break;

            case 3: // Clear
                if (state->text_length > 0) state->modified = true;
                state->text_length = 0;
                move_cursor(state, 0, false);
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                break;

            case 4: // Exit (same path as the close box, so it can ask first)
                dwm_window_send_event(handle, DWM_EVENT_CLOSE);
                break;
            }
        } else if (context_directive == CONTEXT_DIRECTIVE_CANVAS) {
            switch (wparam) {

            case 0: // Cut
                notepad_cut(state);
                break;

            case 1: // Copy
                notepad_copy(state);
                break;

            case 2: // Paste
                notepad_paste(state);
                break;

            case 3: // Delete
                delete_selection(state);
                break;

            case 4: // Select all
                notepad_select_all(state);
                break;
            }

            state->has_preferred_col = false;
            dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        }
        return;
    }
}
