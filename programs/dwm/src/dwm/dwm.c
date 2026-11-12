/*
 * Start-up, main loop and shutdown (dwm.h).
 *
 * Each round of the loop:
 *
 *   1. wait for input or a client program (server.c), until the next timer
 *      is due, or not at all when events are queued or regions are dirty
 *   2. read every waiting input event and route it (input.c): this moves
 *      the cursor, drags windows and icons, and queues window events
 *   3. accept client programs and carry out their requests (server.c)
 *   4. queue a TIMER for each timer that is due (timer.c), time out /
 *      slide notifications (notify.c), show or drop an administrator
 *      prompt (consent.c), bring the taskbar up to date with the windows
 *      (taskbar.c) and walk on with a start menu search (startmenu.c)
 *   5. deliver the queued events (event.c), then send client windows'
 *      events down their pipes
 *   6. repaint and present the dirty regions (compositor.c)
 */

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/security.h>

#include "dwm.h"
#include "compositor.h"
#include "consent.h"
#include "server.h"
#include "internal.h"
#include "../resources/imagelib.h"
#include "../resources/fontlib.h"

static int  input_handle = -1;
static bool running      = false;
static bool started      = false;

static void load_images(void) {
    if (imagelib_count() > 0)
        return;     // The program loaded them already

    // Said as notifications: kept until dwm_init() is done, then shown
    char dir[PATH_MAX];
    if (os_syspath("images", dir, sizeof(dir)) == NULL) {
        dwm_notify_ex(DWM_NOTIFY_ERROR, "No home disk", "System images not loaded", DWM_NOTIFY_STICKY);
        return;
    }
    if (imagelib_load_dir(dir) < 0) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "No images", "%s", strerror(errno));
        return;
    }
    const char* first = NULL;
    size_t skipped = imagelib_skipped(&first);
    if (skipped == 1)
        dwm_notifyf(DWM_NOTIFY_ERROR, "Image skipped", "%s", first);
    else if (skipped > 1)
        dwm_notifyf(DWM_NOTIFY_ERROR, "Images skipped", "%u, first %s", (unsigned)skipped, first);
}

// Fonts are optional: a missing <home>/sys/fonts says nothing
static void load_fonts(void) {
    if (fontlib_count() > 0)
        return;     // The program loaded them already

    char dir[PATH_MAX];
    if (os_syspath("fonts", dir, sizeof(dir)) == NULL)
        return;     // No home disk: load_images() has said so
    int loaded = fontlib_load_dir(dir);
    if (loaded < 0) {
        if (errno != ENOENT)
            dwm_notifyf(DWM_NOTIFY_ERROR, "No fonts", "%s", strerror(errno));
        return;
    }
    const char* first = NULL;
    size_t skipped = fontlib_skipped(&first);
    if (skipped == 1)
        dwm_notifyf(DWM_NOTIFY_ERROR, "Font skipped", "%s", first);
    else if (skipped > 1)
        dwm_notifyf(DWM_NOTIFY_ERROR, "Fonts skipped", "%u, first %s", (unsigned)skipped, first);
}

// The sooner of two timeouts (-1: none)
static int32_t sooner(int32_t a, int32_t b) {
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

int dwm_init(void) {
    if (started)
        return 0;

    // No screen yet, so nowhere to say why: errno only
    sec_mask_t need = SEC_FILE_READ | SEC_GFX | SEC_GFX_SCREEN | SEC_INPUT;
    if (!sec_has(need)) {
        errno = EPERM;
        return -1;
    }

    gfx_info_t info;
    if (gfx_get_info(&info) < 0 || info.width == 0 || info.height == 0) {
        errno = ENODEV;
        return -1;
    }

    load_images();
    load_fonts();
    // DWM_FONT_NAME, else the first font, else the built-in one
    if (!dwm_text_set_font(NULL, DWM_FONT_PX) && dwm_text_truetype())
        dwm_notifyf(DWM_NOTIFY_ERROR, "Font not found", "%s: using %s", DWM_FONT_NAME,
                    dwm_text_font_name());

    if (!compositor_init((int32_t)info.width, (int32_t)info.height))
        return -1;      // errno from the surface
    compositor_set_cursor(imagelib_get("cur_pointer"));

    input_handle = input_open();
    if (input_handle < 0) {
        int err = errno;
        compositor_shutdown();
        errno = err;
        return -1;
    }

    // The pointer starts in the middle of the screen. Move the kernel's
    // pointer there too, then drop what was queued before (the first event
    // is always the old position). A kernel without SYS_INPUT_WARP keeps
    // its own position: the first event moves the cursor there.
    int32_t cx = (int32_t)info.width / 2, cy = (int32_t)info.height / 2;
    if (input_warp(input_handle, cx, cy) == 0) {
        input_event_t stale[INPUT_READ_MAX];
        while (input_read(input_handle, stale, INPUT_READ_MAX) > 0) { }
        input_set_pointer(cx, cy);
    } else {
        compositor_move_cursor(cx, cy);
    }

    // Other programs' windows (os/wm.h). Optional: without it the DWM
    // still runs its own windows.
    server_init();

    // From here notifications show (those kept so far go up now)
    started = true;
    notify_start();

    // Programs asking for admin rights get their prompt from us (and no
    // one else can register for it while we run)
    consent_init();
    return 0;
}

int dwm_run(void) {
    if (!started) {
        errno = EINVAL;
        return -1;
    }

    input_event_t events[INPUT_READ_MAX];
    running = true;

    // The first frame (the whole screen) before anything else
    compositor_render();

    while (running) {
        int32_t timeout = (event_pending() || compositor_dirty() || start_busy())
                        ? 0 : sooner(sooner(sooner(timer_next_timeout(), notify_next_timeout()),
                                            taskbar_next_timeout()), consent_next_timeout());
        if (timeout < 0)
            timeout = IPC_FOREVER;

        ipc_wait_item_t items[IPC_WAIT_MAX];
        items[0].handle = input_handle;
        items[0].events = IPC_READABLE;
        items[0].ready  = 0;
        size_t count = 1 + server_wait_items(items + 1, IPC_WAIT_MAX - 1);
        if (ipc_wait(items, count, timeout) < 0)
            return -1;          // errno from ipc_wait

        int n;
        while ((n = input_read(input_handle, events, INPUT_READ_MAX)) > 0) {
            for (int i = 0; i < n; i++)
                input_route(&events[i]);
        }
        input_recenter();       // With the queue empty, so no stale moves follow
        server_poll();
        icons_notify();
        timer_fire_due();
        notify_tick();
        consent_tick();
        start_tick();

        event_dispatch();
        server_flush();
        taskbar_tick();             // After the events: windows came, went, changed

        // A failed present has no screen to report on; the next frame retries
        compositor_render();
    }
    return 0;
}

bool dwm_warp_pointer(int32_t x, int32_t y) {
    return input_handle >= 0 && input_warp(input_handle, x, y) == 0;
}

void dwm_quit(void) {
    running = false;
}

void dwm_shutdown(void) {
    if (!started)
        return;
    consent_shutdown();
    dwm_menu_close();
    dwm_start_close();
    server_shutdown();          // Client windows first: their programs are told
    window_destroy_all();
    notify_stop();
    event_flush();
    if (input_handle >= 0)
        input_close(input_handle);      // The kernel takes over and repaints
    input_handle = -1;
    compositor_shutdown();
    background_shutdown();
    taskbar_shutdown();
    text_shutdown();
    started = false;
}

int32_t dwm_screen_width(void)  { return compositor_width(); }
int32_t dwm_screen_height(void) { return compositor_height(); }

