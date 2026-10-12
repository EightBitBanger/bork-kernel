#ifndef PROGRAM_EXPLORER_INTERNAL_H
#define PROGRAM_EXPLORER_INTERNAL_H

#include <kernel/programs/explorer/explorer.h>
#include <kernel/vfs/vfs.h>

extern struct Image* icon_folder;
extern struct Image* icon_file;
extern struct Image* icon_document;
extern struct Image* icon_system;
extern struct Image* icon_storage;

extern struct Image* ui_button_back;
extern struct Image* ui_button_new;


extern struct ExplorerWindowState* window_list_head;

WindowHandle explorer_create_instance(const char* title, const char* path);
struct ExplorerWindowState* allocate_window_state(WindowHandle handle);
void free_window_state(WindowHandle handle);

void populate_state_from_vfs(struct ExplorerWindowState* state, const char* target_path);

void callback_handler_explorer(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);

struct Item {
    char name[MAX_TITLE_LEN];          // File/alias name
    char path[MAX_PATH_LEN];           // Absolute path for VFS operations
    uint16_t icon_index;               // Icon representing this item
    uint32_t knode;                    // Legacy compatibility address
    uint32_t fs_dir;                   // Legacy compatibility address
};

struct ExplorerWindowState {
    WindowHandle handle;
    EditFieldHandle edit_handle;
    uint16_t edit_width;
    
    uint16_t win_width;
    uint16_t win_height;
    
    uint16_t total_items;
    
    int32_t context_item_index;        // Tracks right-clicked item (-1 means none/blank area)
    uint32_t context_directive;        // Tracks the type of menu that was summoned
    
    uint32_t knode_current;            
    uint32_t fs_current;               // Non-zero if inside a mounted FS for UI path coloring
    
    struct ExplorerWindowState* next;
    
    struct Item items[MAX_ITEMS];
    
    char window_title[MAX_TITLE_LEN];  // Current directory name as the window title
    char path[MAX_PATH_LEN];           // Display path
    char full_path[MAX_PATH_LEN];      // Absolute VFS path
    uint16_t knode_path_len;           // Length of virtual base path for split rendering
};

#endif
