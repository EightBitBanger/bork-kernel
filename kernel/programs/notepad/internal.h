#ifndef PROGRAM_NOTEPAD_INTERNAL_H
#define PROGRAM_NOTEPAD_INTERNAL_H

#include <kernel/programs/notepad/notepad.h>

#define MAX_TEXT_LEN                 2048

#define CONTEXT_DIRECTIVE_CANVAS        0
#define CONTEXT_DIRECTIVE_FILE          1

extern uint8_t context_directive;

struct NotepadWindowState {
    WindowHandle handle;
    char file_path[DWM_MAX_PATH_LEN];
    
    uint16_t win_width;
    uint16_t win_height;
    
    char window_title[MAX_TITLE_LEN];
    char* text_buffer;
    uint32_t text_capacity;
    uint32_t text_length;
    
    uint32_t cursor_index;
    
    // Selection is [min(sel_anchor, cursor_index), max(...)). When the two are
    // equal nothing is selected. Every cursor move either drags the anchor
    // along (plain move) or leaves it in place (shift / mouse drag).
    uint32_t sel_anchor;
    bool     mouse_selecting;     // Left button held after clicking the text
    
    // Column remembered across Up/Down so the caret doesn't drift left
    // when it passes through short lines
    bool     has_preferred_col;
    uint16_t preferred_col;

    // Unsaved changes / exit confirmation
    bool         modified;            // Edited since it was loaded or last saved
    bool         close_confirmed;     // User answered the prompt: let CLOSE through
    WindowHandle confirm_window;      // Open "save changes?" dialog, or 0

    struct NotepadWindowState* next;
};

extern struct NotepadWindowState* notepad_window_list_head;

WindowHandle notepad_create_instance(const char* title, const char* path);
struct NotepadWindowState* allocate_notepad_window_state(WindowHandle handle, const char* path);
void free_notepad_window_state(WindowHandle handle);

void callback_handler_notepad(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);

#endif
