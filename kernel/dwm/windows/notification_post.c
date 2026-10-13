// dwm_notify_post: create a notification from any thread.
//
// Self-contained: it only needs dwm_invoke() and dwm_notify_ex(). Add it to
// the build as is, or paste it into notification.c.

#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

struct NotifyPost {
    DWMNotifyKind kind;
    uint32_t      timeout_ms;
    char          title[64];
    char          text[160];
};

// DWM thread, DWM lock held (queued with dwm_invoke)
static void notify_post_run(void* arg) {
    struct NotifyPost* post = (struct NotifyPost*)arg;
    dwm_notify_ex(post->kind, post->title, post->text, post->timeout_ms);
    free(post);
}

bool dwm_notify_post(DWMNotifyKind kind, const char* title, const char* text, uint32_t timeout_ms) {
    struct NotifyPost* post = (struct NotifyPost*)malloc(sizeof(struct NotifyPost));
    if (post == NULL) return false;

    memset(post, 0, sizeof(struct NotifyPost));
    post->kind       = kind;
    post->timeout_ms = timeout_ms;
    if (title != NULL) strncpy(post->title, title, sizeof(post->title) - 1);
    if (text  != NULL) strncpy(post->text,  text,  sizeof(post->text) - 1);

    // Not queued: fn is not called, so we still own the block
    if (!dwm_invoke(notify_post_run, post)) {
        free(post);
        return false;
    }
    return true;
}
