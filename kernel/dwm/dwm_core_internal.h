#ifndef DWM_INTERNAL_CORE_CONPONENTS_H
#define DWM_INTERNAL_CORE_CONPONENTS_H

#include <kernel/dwm/dwm.h>
#include <kernel/dwm/objects/window_button.h>
#include <kernel/dwm/dwm_context_menu.h>

#include <kernel/dwm/dwm_platform.h>
#include <kernel/dwm/flags.h>

#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>

#define MAX_CONTEXT_MENUS 8

// ==========================================
// Locking and threads (dwm_thread.c)
// ==========================================
//
// dwm_lock guards ALL DWM state. It is recursive for the thread that holds
// it. See the top of dwm_thread.c for the full threading model and
// kernel_lock.h for the lock order.

extern mutex_t dwm_mutex;           // Underlying mutex: use dwm_lock()/dwm_unlock()

void dwm_lock(void);
void dwm_unlock(void);
bool dwm_lock_held(void);
bool dwm_on_thread(void);           // True on the DWM thread
void dwm_wake(void);                // Make the DWM thread run a frame soon

// Scoped lock: takes dwm_lock and releases it on every return path.
// Put it first in a public function body.
static inline int  dwm_guard_acquire(void)   { dwm_lock(); return 1; }
static inline void dwm_guard_release(int* g) { (void)g; dwm_unlock(); }
#define DWM_GUARD() \
    int _dwm_guard __attribute__((cleanup(dwm_guard_release), unused)) = dwm_guard_acquire()

// Queue a kernel event (KEVENT_*) from DWM code. Delivered by the DWM thread
// after it releases dwm_lock. Safe from any thread. DWM code must use this
// instead of kernel_event_send().
void dwm_kernel_event_send(uint32_t type, const char* a, const char* b);

struct DWMWorkspace {
    struct list_node* window_head;
    struct list_node* window_tail;
    
    struct list_node* icon_head;
    struct list_node* icon_tail;
    
    struct map_node* resource_head;
    struct map_node* resource_tail;
    
    uint32_t next_window_id;
    uint32_t next_edit_field_id;
    
    // Focus tracking
    bool desktop_focused;           // True after a click on empty desktop (no window has focus)
    WindowHandle last_focus;        // Focus as seen on the previous frame (for focus events)
};

struct DWMTaskbar {
    WindowHandle window;
    
    uint16_t height;
};

struct DWMDragDrop {
    struct WindowObject* dragged_window;
    int drag_offset_x;
    int drag_offset_y;
    
    struct IconObject* dragged_icon;
    int icon_drag_offset_x;
    int icon_drag_offset_y;
    
    // Add click origin coordinates for hysteresis
    int drag_start_x;
    int drag_start_y;
    bool is_dragging;
    
    struct WindowObject* dragged_resizing;
    int resize_offset_x;
    int resize_offset_y;
    
    // Mouse capture (see DWM_EVENT_MOUSE_MOVE / DWM_EVENT_MOUSE_UP)
    WindowHandle captured_window;   // 0 when nothing is captured
    Point capture_last;             // Last position sent to the captured window
};

struct DWMContext {
    struct WindowContext window_context;
    struct WindowObject* event_window;
    
    struct IconObject* focused_icon;
    struct IconObject* last_focused_icon;
    
    uint32_t last_icon_click_time;  // Double click timing
};

struct DWMInput {
    uint16_t last_key_pressed;
    
    Point mouse_last;
    
    bool last_left_button_pressed;
    bool last_right_button_pressed;
};

struct DWMContextMenu {
    struct ContextMenu menus[MAX_CONTEXT_MENUS];
    
    uint8_t menu_count;
    uint16_t menu_directive;
    struct WindowObject* handle;
};

struct DWMImages {
    struct Image current_cursor;
    
};

struct DWMCascade {
    uint16_t x;
    uint16_t y;
    
    uint16_t h;
    uint16_t w;
    
    uint16_t max;
};

extern struct DWMTheme theme;

extern struct DWMWorkspace    workspace;
extern struct DWMContext      context;
extern struct DWMTaskbar      taskbar;
extern struct DWMDragDrop     dragdrop;
extern struct DWMInput        input;
extern struct DWMContextMenu  ctxmenu;
extern struct DWMImages       images;
extern struct DWMCascade      cascade;


// UI

bool dwm_create_context_menu(int x, int y, uint32_t directive, const char* items[], int item_count);

void window_add_button(struct WindowObject* window, int16_t x, int16_t y, uint16_t width, uint16_t height, 
                       uint16_t event, struct Image* sprite);

// Built in event handlers

void callback_message_box_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);
void callback_message_error_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);

void callback_properties_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);
void callback_deletion_dialog_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);
void callback_taskbar_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);
void callback_start_menu_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);
void callback_filecopy_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);
void callback_filedelete_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);

// Paths and desktop icons for files arriving in a folder (windows/filecopy.c)

// First free "<dir>/<name>", "<dir>/<name (2)>", ... (names kept within
// VFS_NAME_MAX). Writes the full path and the bare name.
bool dwm_path_unique(const char* dir, const char* name, char* out_path, size_t path_size, char* out_name);

// "<home>/usr/desktop". False when there is no home device.
bool dwm_desktop_get_directory(char* out, size_t size);

// Desktop icon for a new file or folder at (x, y), or on the next free grid
// spot when either is negative
void dwm_desktop_add_item_icon(const char* path, const char* name, int x, int y);

// Remove desktop icons for a path that is gone (and anything below it)
void dwm_desktop_forget_path(const char* path);

// Post DWM_EVENT_REFRESH to every open window (explorers re-read their
// folder) and send KEVENT_DWM_REFRESH. Use after anything changes on disk.
void dwm_refresh_all_windows(void);

// Internal routines

void dwm_theme_init(void);   // dwm_theme.c: load the default theme
void dwm_notification_tick(void);   // windows/notification.c: timeouts and sliding, once per frame
void dwm_start_menu_tick(void);     // windows/start_menu.c: slide in / out, once per frame

// Slide animation (dwm_aux.c), shared by notifications and the start menu.
// dwm_slide_step: next position on the way from cur to target, covering
// 1/divisor of the remaining distance per frame (ease out, at least 2 px).
// dwm_window_place: move a top-level window, repainting old and new spots.
int  dwm_slide_step(int cur, int target, int divisor);
void dwm_window_place(struct WindowObject* window, int x, int y);

struct WindowObject* dwm_allocate_window(WindowClass w_class, uint16_t w_style, WindowProcedure proc);

void dwm_resource_load(const char* name, void* resource);
void* dwm_resource_find(const char* name);
void dwm_resource_unload(const char* name);
void dwm_resource_sprite_load(const char* resource_name, const struct Sprite* sprite);

void dwm_draw_desktop(const struct WindowContext* ctx);
void dwm_draw_window(struct WindowObject* window_handle);
void dwm_draw_redraw(int16_t x, int16_t y, int16_t w, int16_t h);

void dwm_update_mouse(struct WindowContext* ctx);
void dwm_update_window_dragging(struct WindowContext* ctx);
void dwm_update_icon_dragging(struct WindowContext* ctx);
void dwm_update_window_resizing(struct WindowContext* ctx);
void dwm_update_window_capture(struct WindowContext* ctx);

void dwm_sync_child_positions(struct WindowObject* parent);
void dwm_calculate_icon_bounds(struct IconObject* icon);
void dwm_invalidate_region(int16_t x, int16_t y, int16_t w, int16_t h);
void dwm_get_absolute_position(struct WindowObject* window, int* out_x, int* out_y);
void dwm_cascade_child_positions(struct WindowObject* parent);
void dwm_process_window_events(struct WindowObject* window);
void dwm_process_context_menu_events(struct WindowContext* ctx, uint16_t index);

void dwm_set_focus(struct WindowObject* target);
void dwm_set_desktop_focus(void);
struct WindowObject* dwm_get_focused_window(void);
void dwm_process_focus_change(void);
void dwm_calculate_flush_region(struct WindowContext* ctx);

struct WindowObject* dwm_get_window_by_id(uint32_t id);

bool rects_intersect(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2);

// Desktop icon rename (dwm_rename.c)
bool dwm_desktop_rename_begin(struct IconObject* icon);
bool dwm_desktop_rename_commit(void);
void dwm_desktop_rename_cancel(void);
bool dwm_desktop_rename_active(void);
struct IconObject* dwm_desktop_rename_icon(void);
void dwm_desktop_rename_forget_icon(struct IconObject* icon);
bool dwm_desktop_rename_handle_click(const struct WindowContext* ctx);
void dwm_desktop_rename_draw(const struct WindowContext* ctx);

struct WindowObject* dwm_get_root_parent(struct WindowObject* window);

void dwm_resize_window_buffer(struct WindowObject* window, int new_w, int new_h);
void dwm_upload_window_buffer_to_backbuffer(struct WindowObject* window, uint32_t* frame_buffer, uint32_t screen_stride, 
                                            int clip_x, int clip_y, int clip_w, int clip_h);

#endif
