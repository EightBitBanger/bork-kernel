/*
 * Window demo: asks the dwm for a resizable window and keeps it drawn.
 *
 *   click        drop a dot
 *   type         text along the bottom (Backspace erases)
 *   resize       the drawing follows the window size
 *   close box    quits
 *
 * Start it after the dwm (it waits a few seconds for it to appear).
 * Needs SEC_IPC and SEC_GFX, both in the default clearance; SEC_CONSOLE for
 * messages. It never touches the screen itself.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/wm.h>

#define DOTS     64
#define TEXT_MAX 120

static int32_t  dot_x[DOTS], dot_y[DOTS];
static unsigned dots;
static char     text[TEXT_MAX + 1];
static int32_t  mouse_x = -1, mouse_y = -1;
static bool     focused;

static void draw(wm_window_t win) {
    gfx_surface_t* s = wm_window_surface(win);
    int32_t w = gfx_surface_width(s), h = gfx_surface_height(s);

    gfx_fill_gradient(s, 0, 0, w, h, GFX_RGB(18, 22, 44), GFX_RGB(44, 20, 52), GFX_GRADIENT_VERTICAL);

    char line[96];
    snprintf(line, sizeof(line), "pid %d   %dx%d   %u dots", getpid(), w, h, dots);
    gfx_draw_text(s, 8, 8, line, GFX_RGB(220, 230, 255));
    if (mouse_x >= 0) {
        snprintf(line, sizeof(line), "mouse %d,%d", mouse_x, mouse_y);
        gfx_draw_text(s, 8, 20, line, GFX_RGB(150, 160, 200));
    }

    for (unsigned i = 0; i < dots && i < DOTS; i++)
        gfx_fill_circle(s, dot_x[i], dot_y[i], 5, GFX_RGB(240, 190, 60));

    gfx_fill_rect(s, 0, h - 20, w, 20, focused ? GFX_RGB(20, 70, 30) : GFX_RGB(40, 40, 50));
    gfx_draw_text(s, 6, h - 14, text[0] ? text : (focused ? "type something" : "click to focus"),
                  GFX_RGB(80, 255, 80));

    wm_window_commit(win, NULL);
}

int main(void) {
    if (wm_connect(5000) < 0) {
        printf("wm_demo: no window manager: %s\n", strerror(errno));
        return 1;
    }

    char title[64];
    snprintf(title, sizeof(title), "wm_demo (pid %d)", getpid());
    wm_window_desc_t desc = {
        .title = title, .x = WM_POS_DEFAULT, .y = WM_POS_DEFAULT,
        .width = 360, .height = 220, .min_width = 160, .min_height = 80,
        .style = WM_STYLE_RESIZABLE,
    };
    wm_window_t win = wm_window_create(&desc);
    if (win == 0) {
        printf("wm_demo: no window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    draw(win);      // First commit: the window appears

    wm_event_t ev;
    bool running = true;
    while (running && wm_wait_event(&ev, -1) > 0) {
        switch (ev.type) {
            case WM_EVENT_CLOSE:
            case WM_EVENT_DESTROYED:
                running = false;
                break;
            case WM_EVENT_MOUSE_DOWN:
                dot_x[dots % DOTS] = ev.x;
                dot_y[dots % DOTS] = ev.y;
                dots++;
                draw(win);
                break;
            case WM_EVENT_MOUSE_MOVE:
                mouse_x = ev.x;
                mouse_y = ev.y;
                draw(win);
                break;
            case WM_EVENT_KEY: {
                size_t n = strlen(text);
                if (ev.key == KEY_BACKSPACE && n > 0)
                    text[n - 1] = '\0';
                else if (ev.key >= 0x20 && ev.key < 0x7F && n < TEXT_MAX)
                    text[n] = (char)ev.key, text[n + 1] = '\0';
                draw(win);
                break;
            }
            case WM_EVENT_FOCUS_GAINED:
            case WM_EVENT_FOCUS_LOST:
                focused = (ev.type == WM_EVENT_FOCUS_GAINED);
                draw(win);
                break;
            case WM_EVENT_RESIZED:
                draw(win);      // The surface already has the new size
                break;
        }
    }
    if (running)
        printf("wm_demo: connection lost: %s\n", strerror(errno));

    wm_disconnect();
    return 0;
}
