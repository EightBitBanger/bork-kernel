#ifndef _MESSAGE_CONFIRM_WINDOW_H_
#define _MESSAGE_CONFIRM_WINDOW_H_

#include <kernel/dwm/dwm.h>

// Three-button confirmation box: [yes_label] [no_label] [Cancel]
//
// The answer arrives through `callback`, on the DWM thread with the DWM lock
// held (like a window procedure), exactly once per dialog:
//
//   DWM_CONFIRM_YES     first button, or Enter
//   DWM_CONFIRM_NO      second button
//   DWM_CONFIRM_CANCEL  Cancel, Esc, the close box, or the dialog being
//                       destroyed any other way
//
// The dialog closes itself after answering.

typedef enum {
    DWM_CONFIRM_YES    = 0,
    DWM_CONFIRM_NO     = 1,
    DWM_CONFIRM_CANCEL = 2,
} DWMConfirmResult;

typedef void (*DWMConfirmCallback)(DWMConfirmResult result, void* user);

// yes_label / no_label may be NULL ("Yes" / "No"). Labels longer than
// 12 characters are cut. Returns 0 if the dialog could not be created
// (the callback is then never called).
WindowHandle dwm_summon_message_confirm(const char* title, const char* message,
                                        const char* yes_label, const char* no_label,
                                        DWMConfirmCallback callback, void* user);

#endif
