#include <stdio.h>
#include <stdbool.h>

#include <kernel/malloc.h>

#include <kernel/kernel.h>
#include <kernel/dwm/dwm.h>
#include <kernel/events.h>

#include <kernel/programs/notepad/internal.h>
#include <kernel/programs/notepad/notepad.h>

#include <kernel/util/string.h>
#include <kernel/util/parser.h>

uint8_t context_directive = 0; // Context menu directive

struct NotepadWindowState* notepad_window_list_head = NULL;

WindowHandle notepad_main(const char* arguments) {
    char window_title[DWM_MAX_TITLE_LEN] = "Untitled - Notepad";
    
    // Fallback assignment to capture target document paths if passed
    if (arguments != NULL && arguments[0] != '\0') {
        parse_get_filename(arguments, window_title, 128);
        window_title[DWM_MAX_TITLE_LEN - 1] = '\0';
    }
    
    WindowHandle handle = notepad_create_instance(window_title, arguments);
    return handle;
}

struct NotepadWindowState* allocate_notepad_window_state(WindowHandle handle, const char* path) {
    struct NotepadWindowState* new_node = (struct NotepadWindowState*)malloc(sizeof(struct NotepadWindowState));
    if (!new_node) return NULL;
    
    memset(new_node, 0, sizeof(struct NotepadWindowState));
    new_node->handle = handle;
    new_node->win_width = 400;
    new_node->text_length = 0;
    
    strncpy(new_node->file_path, path, DWM_MAX_PATH_LEN);
    
    new_node->next = notepad_window_list_head;
    notepad_window_list_head = new_node;
    
    uint8_t permissions = 0;
    vfs_get_permissions(path, &permissions);
    
    if (!(permissions & VFS_PERMISSION_READ)) {
        dwm_notify("Notepad", "Cannot read this file: access denied", DWM_NOTIFY_LONG_MS);
        // Allocate a small default buffer for empty files
        new_node->text_capacity = 2048;
        new_node->text_buffer = (char*)malloc(new_node->text_capacity);
        if (new_node->text_buffer) new_node->text_buffer[0] = '\0';
        return new_node;
    }
    
    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) {
        new_node->text_capacity = 2048;
        new_node->text_buffer = (char*)malloc(new_node->text_capacity);
        if (new_node->text_buffer) new_node->text_buffer[0] = '\0';
        return new_node;
    }
    
    uint32_t size = vfs_get_size(file);
    
    new_node->text_capacity = size + 2048; 
    new_node->text_buffer = (char*)malloc(new_node->text_capacity);
    if (!new_node->text_buffer) {
        vfs_close(file);
        return new_node;
    }
    
    vfs_read(file, new_node->text_buffer, size);
    vfs_close(file);
    
    new_node->text_length = size;
    
    return new_node;
}

void free_notepad_window_state(WindowHandle handle) {
    struct NotepadWindowState* current = notepad_window_list_head;
    struct NotepadWindowState* previous = NULL;
    
    while (current != NULL) {
        if (current->handle == handle) {
            if (previous == NULL) {
                notepad_window_list_head = current->next;
            } else {
                previous->next = current->next;
            }
            // Free the dynamic text buffer first to avoid leaks
            if (current->text_buffer) {
                free(current->text_buffer);
            }
            free(current);
            return;
        }
        previous = current;
        current = current->next;
    }
}

WindowHandle notepad_create_instance(const char* title, const char* path) {
    struct NotepadWindowState* state = allocate_notepad_window_state(0, path);
    if (state == NULL) return 0;
    
    strncpy(state->window_title, title, MAX_TITLE_LEN - 1);
    state->window_title[MAX_TITLE_LEN - 1] = '\0';
    
    WindowClass wclass;
    memset(&wclass, 0x00, sizeof(WindowClass));
    wclass.x = 250;
    wclass.y = 180;
    wclass.width  = 660;
    wclass.height = 440;
    wclass.max_width  = 0;
    wclass.max_height = 0;
    
    size_t title_length = strnlen(title, DWM_MAX_TITLE_LEN);
    strncpy(wclass.title, title, title_length);
    wclass.title[title_length] = '\0';
    
    WindowHandle window = dwm_create_window(wclass, DWM_WSTYLE_RESIZEABLE, callback_handler_notepad);
    state->handle = window;
    
    dwm_window_set_focus(window);
    return window;
}
