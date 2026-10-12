#ifndef _DIALOG_FILE_H_
#define _DIALOG_FILE_H_

#include <kernel/dwm/dwm.h>

#define DIALOG_FILE_MODE_SAVE 0
#define DIALOG_FILE_MODE_LOAD 1

// Callback signature triggered when the user accepts or cancels the dialog
typedef void (*FileDialogCallback)(const char* path, bool cancelled);

WindowHandle dwm_summon_file_dialog(const char* title, const char* initial_path, uint8_t mode, FileDialogCallback callback);

void callback_file_dialog_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam);

#endif
