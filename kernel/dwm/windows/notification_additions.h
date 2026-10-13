// ==========================================================================
// MERGE INTO dwm/windows/notification.h
//
// notification.h was not in the source dump, so these declarations are
// meant to be pasted into it (next to dwm_notify) rather than replace it.
// ==========================================================================

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    DWM_NOTIFY_INFO  = 0,       // Existing look (what dwm_notify shows)
    DWM_NOTIFY_ERROR = 1,       // Red box: theme.notify.error_*
} DWMNotifyKind;

// timeout_ms value: stay on screen until the user clicks it
#define DWM_NOTIFY_STICKY   0xFFFFFFFFu

// Like dwm_notify, with a style and an optional sticky timeout.
// Takes the DWM lock (same rules as dwm_notify).
bool dwm_notify_ex(DWMNotifyKind kind, const char* title, const char* text, uint32_t timeout_ms);

// Same, safe from ANY thread without touching the DWM lock: the strings are
// copied and the notification is created on the DWM thread next frame
// (dwm_invoke). Returns false if it could not be queued.
bool dwm_notify_post(DWMNotifyKind kind, const char* title, const char* text, uint32_t timeout_ms);
