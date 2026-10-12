#ifndef _WINDOW_MANAGER_H_
#define _WINDOW_MANAGER_H_

#include <stdbool.h>
#include <stddef.h>

#include <kernel/scheduler/scheduler.h>

#include <kernel/dwm/rendering/image.h>

#include <kernel/dwm/images/icon.h>
#include <kernel/dwm/images/ui.h>
#include <kernel/dwm/images/cursor.h>
#include <kernel/dwm/images/images.h>

#include <kernel/dwm/objects/icon_object.h>

#include <kernel/dwm/objects/window_context.h>
#include <kernel/dwm/objects/window_object.h>
#include <kernel/dwm/objects/window_edit.h>
#include <kernel/dwm/objects/window_handle.h>
#include <kernel/dwm/objects/window_class.h>

#include <kernel/dwm/objects/context_menu.h>
#include <kernel/dwm/rendering/sprite.h>

#include <kernel/dwm/style_flags.h>
#include <kernel/dwm/configuration.h>
#include <kernel/dwm/theme.h>

#include <kernel/dwm/windows/properties.h>
#include <kernel/dwm/windows/message_box.h>
#include <kernel/dwm/windows/message_error.h>
#include <kernel/dwm/windows/dialog_delete.h>
#include <kernel/dwm/windows/dialog_file.h>
#include <kernel/dwm/windows/start_menu.h>
#include <kernel/dwm/windows/filecopy.h>
#include <kernel/dwm/windows/filedelete.h>
#include <kernel/dwm/windows/message_confirm.h>
#include <kernel/dwm/windows/notification.h>

#include <kernel/dwm/dwm_dispatch.h>
#include <kernel/dwm/dwm_clipboard.h>

typedef void(*WindowProcedure)(WindowHandle, wEvent, uint32_t wparam, int32_t lparam);

// ==========================================
// Threading
// ==========================================
//
// The DWM runs on its own thread (dwm_start) and guards its state with its
// own lock. From another thread:
//
//   - dwm_post_message / dwm_window_send_event are always safe
//   - the other public functions take the DWM lock themselves; do not call
//     them while holding a lock the DWM takes after its own (filecopy, VFS,
//     clipboard), see <kernel/kernel_lock.h>
//   - worker threads should prefer dwm_invoke(): fn(arg) then runs on the
//     DWM thread, inside a frame, with the DWM lock held
//
// Window procedures always run on the DWM thread with the DWM lock held.
// They must not block (event_wait, thread_sleep, long disk work): the whole
// desktop stops while they do.

typedef void (*DWMInvokeFn)(void* arg);

// Build the desktop (call once, on the boot thread, before dwm_start)
void dwm_initiate(void);

// Start the DWM thread. Returns it (or NULL on failure).
ThreadBlock* dwm_start(ThreadPriority priority);
ThreadBlock* dwm_get_thread(void);

// Run fn(arg) on the DWM thread. Returns false if it could not be queued
// (fn is then NOT called, so the caller still owns arg).
bool dwm_invoke(DWMInvokeFn fn, void* arg);

// Queue a kernel event (KEVENT_*) for the kernel thread. Safe from any
// thread, and the way window procedures must send one: they run with the DWM
// lock held, and kernel_event_send() would take kernel_lock after it
// (inverted lock order). Delivered right after the current frame.
void dwm_kernel_event_send(uint32_t type, const char* a, const char* b);

// Programs (dwm_launch.c). A regular file with VFS_PERMISSION_EXECUTE (the
// 'x' in Properties) is a program: openers should launch it instead of
// handing it to notepad. dwm_launch_program loads it on a worker thread and
// reports a failure as a notification. Both take the DWM lock.
bool dwm_path_is_executable(const char* path);
bool dwm_launch_program(const char* path);

// One frame. DWM thread only, DWM lock held.
void dwm_update(void);

// Windows

WindowHandle dwm_create_window(WindowClass wclass, uint16_t wstyle, WindowProcedure wproc);
void dwm_destroy_window(WindowHandle window_handle);

// Icons

int8_t dwm_create_folder(uint16_t x, uint16_t y, const char* name, const char* path);
int8_t dwm_create_file(uint16_t x, uint16_t y, const char* name, const char* path);
int8_t dwm_create_mount(uint16_t x, uint16_t y, const char* name, const char* path);

struct IconObject* dwm_create_icon(uint16_t x, uint16_t y, uint16_t width, uint16_t height, struct Image* sprite, uint16_t icon_index);
void dwm_destroy_icon(struct IconObject* icon);

// Context functions

void dwm_summon_context_menu(WindowHandle window, uint16_t x, uint16_t y, const char** options, uint16_t number_of_items);

// Resource management

void* dwm_resource_find(const char* name);

// Image resources (shared by boot.c, which saves them, and dwm_initiate,
// which loads them)

struct DWMBuiltinImage {
    const char*          name;      // Resource name and file name
    const struct Sprite* sprite;    // Compiled-in copy (save source / fallback)
};

extern const struct DWMBuiltinImage dwm_builtin_images[];
extern const uint32_t dwm_builtin_image_count;

// Directory the DWM images live in: "<home>/sys/images".
// Returns false when no home device was found.
bool dwm_get_image_directory(char* out, size_t size);

// Window resource management

uint8_t dwm_window_resource_add(WindowHandle handle, const char* name, void* resource);
uint8_t dwm_window_resource_remove(WindowHandle handle, void* resource);
uint32_t dwm_window_resource_get_count(WindowHandle handle);
void* dwm_window_resource_get_by_index(WindowHandle handle, uint32_t index);
void* dwm_window_resource_get_by_name(WindowHandle handle, const char* name);
void dwm_window_resource_free_all(WindowHandle handle);

// Primitive drawing

void dwm_draw_line(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
void dwm_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
void dwm_draw_rect_filled(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
void dwm_draw_rect_filled_gradient_vertical(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color_low, uint32_t color_high);
void dwm_draw_rect_filled_gradient_horizontal(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color_low, uint32_t color_high);

void dwm_draw_text(int16_t x, int16_t y, const char* text, uint32_t color);
void dwm_draw_redraw(int16_t x, int16_t y, int16_t w, int16_t h);
void dwm_draw_sprite(int16_t x, int16_t y, struct Image* image);

// Window objects
EditFieldHandle dwm_window_add_edit_field(WindowHandle handle, uint16_t x, uint16_t y, uint16_t width);

// Edit box
bool dwm_window_edit_text(WindowHandle handle, EditFieldHandle edit_field_handle, const char* text);
bool dwm_window_edit_visible(WindowHandle handle, EditFieldHandle edit_field_handle, bool enable);
bool dwm_window_edit_insert(WindowHandle handle, EditFieldHandle edit_field_handle, const char* text);
bool dwm_window_edit_backspace(WindowHandle handle, EditFieldHandle edit_field_handle);
bool dwm_window_edit_cursor_set_pos(WindowHandle handle, EditFieldHandle edit_field_handle, uint16_t position);
bool dwm_window_edit_set_pos(WindowHandle handle, EditFieldHandle edit_field_handle, uint16_t x, uint16_t y);
bool dwm_window_edit_set_width(WindowHandle handle, EditFieldHandle edit_field_handle, uint16_t width);
size_t dwm_window_edit_get_len(WindowHandle handle, EditFieldHandle edit_field_handle);
bool dwm_window_edit_get_text(WindowHandle handle, EditFieldHandle edit_field_handle, char* name_buffer, size_t buffer_size);

// Window settings

void dwm_window_set_parent(WindowHandle child, WindowHandle parent);
WindowHandle dwm_window_get_parent(WindowHandle window);

uint16_t dwm_window_get_width(WindowHandle handle);
uint16_t dwm_window_get_height(WindowHandle handle);

uint8_t dwm_window_set_name(WindowHandle handle, const char* name);

void dwm_set_cursor(uint32_t* sprite, int16_t width, int16_t height);

uint16_t dwm_get_titlebar_height(WindowHandle handle);

void dwm_window_set_focus(WindowHandle handle);

WindowHandle dwm_window_get_focus(void);

// Send an even to a specific window
void dwm_window_send_event(WindowHandle handle, wEvent event);

// Call from a window procedure while it handles DWM_EVENT_CLOSE to keep the
// window open (no DESTROY, no destroy). Use it to ask about unsaved changes
// first, then send DWM_EVENT_CLOSE again once the user agreed.
void dwm_window_cancel_close(WindowHandle handle);

// Get number of open windows
uint32_t dwm_window_get_count(void);

// Clipboard: see dwm_clipboard.h (included above)

// Set the current key input
void dwm_set_keyboard_char(uint16_t ch);

// Offer a key to the desktop icon rename box. Returns true when a desktop
// rename is in progress and the key was consumed; otherwise the caller
// should deliver the key to the focused window as usual.
bool dwm_desktop_rename_key(uint16_t key);

// Desktop icon layout (<home>/usr/icons)

// Move existing desktop icons to their saved positions. Call after the
// desktop icons have been created. Returns false if there is no saved layout.
bool dwm_desktop_layout_load(void);

// Write the current position of every desktop icon to <home>/usr/icons
bool dwm_desktop_layout_save(void);

// Announce a newly created file or folder: adds a desktop icon (next free
// grid spot) when it is in the desktop folder and has none yet, then
// refreshes every open explorer window
void dwm_notify_item_created(const char* path);

#endif
