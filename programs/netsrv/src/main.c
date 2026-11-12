/*
 * netsrv: the network icon on the taskbar, as a small service.
 *
 * The desktop starts it (<home>/sys/services/netsrv) with only the
 * permissions it needs; it has no window of its own until its icon is
 * clicked. See nettray.c for what it shows and why.
 *
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <os/wm.h>
#include "nettray.h"

int main(void) {
    if (wm_connect(10000) < 0)
        return 1;           // No desktop: nowhere to show anything
    if (nettray_start() < 0) {
        int err = errno;
        if (err == EEXIST)
            wm_notify_ex(WM_NOTIFY_INFO, "Wi-Fi", "The Wi-Fi service is already running", WM_NOTIFY_DEFAULT_MS);
        else
            wm_notify_ex(WM_NOTIFY_ERROR, "Wi-Fi icon", err == EMFILE || err == ENOSPC ? "The tray is full" : strerror(err),
                         WM_NOTIFY_DEFAULT_MS);
        wm_disconnect();
        return err == EEXIST ? 0 : 1;
    }

    wm_event_t ev;
    bool running = true;
    while (running) {
        int r = wm_wait_event(&ev, nettray_timeout());
        if (r < 0)
            break;          // The desktop went away
        if (r > 0)
            running = nettray_handle(&ev);
        nettray_tick();
    }

    nettray_stop();
    wm_disconnect();
    return 0;
}
