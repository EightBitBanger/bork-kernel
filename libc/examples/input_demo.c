/*
 * Input demo: a full-screen background with a box you can drag with the
 * mouse. Exercises the input syscalls (os/input.h) and direct-to-screen
 * graphics.
 *
 *   drag the box     left button
 *   nudge the box    arrow keys (Shift: bigger steps)
 *   quit             Esc or q
 *
 * Taking the input pauses the window manager; it repaints the desktop when
 * this program exits.
 *
 * Needs SEC_GFX | SEC_GFX_SCREEN | SEC_INPUT (SEC_CONSOLE for messages).
 */

#include <stdbool.h>
#include <stdio.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/security.h>

#define BG          GFX_RGB(32, 92, 128)
#define BOX_FILL    GFX_RGB(240, 170, 60)
#define BOX_HOVER   GFX_RGB(250, 200, 100)
#define BOX_DRAG    GFX_RGB(255, 225, 140)
#define BOX_EDGE    GFX_RGB(60, 30, 10)
#define SHADOW      GFX_ARGB(90, 0, 0, 0)
#define TEXT        GFX_RGB(230, 240, 250)

#define BOX_W       120
#define BOX_H       80
#define SHADOW_OFF  6
#define CURSOR_W    9
#define CURSOR_H    13
#define STATUS_H    28

typedef struct {
    int32_t x, y;
} point_t;

static gfx_surface_t* screen;
static int32_t screen_w, screen_h;

static point_t box;                 // Top-left corner
static point_t mouse;
static uint8_t buttons;
static bool    dragging;
static point_t grab;                // Mouse offset inside the box while dragging
static uint32_t events_seen;

static bool inside_box(point_t p) {
    return p.x >= box.x && p.x < box.x + BOX_W && p.y >= box.y && p.y < box.y + BOX_H;
}

static gfx_rect_t box_rect(void) {
    return gfx_rect(box.x, box.y, BOX_W + SHADOW_OFF, BOX_H + SHADOW_OFF);
}

static gfx_rect_t cursor_rect(void) {
    return gfx_rect(mouse.x, mouse.y, CURSOR_W, CURSOR_H);
}

static gfx_rect_t status_rect(void) {
    return gfx_rect(0, 0, screen_w, STATUS_H);
}

static void clamp_box(void) {
    if (box.x < 0) box.x = 0;
    if (box.y < STATUS_H) box.y = STATUS_H;
    if (box.x > screen_w - BOX_W) box.x = screen_w - BOX_W;
    if (box.y > screen_h - BOX_H) box.y = screen_h - BOX_H;
}

// A plain arrow pointer: white with a black edge
static void draw_cursor(void) {
    for (int32_t row = 0; row < CURSOR_H; row++) {
        int32_t w = (row < CURSOR_W) ? row + 1 : CURSOR_W - (row - CURSOR_W + 1) * 2;
        if (w <= 0)
            break;
        gfx_draw_hline(screen, mouse.x, mouse.y + row, w, GFX_COLOR_BLACK);
        if (w > 2)
            gfx_draw_hline(screen, mouse.x + 1, mouse.y + row, w - 2, GFX_COLOR_WHITE);
    }
}

// Draw the whole scene, clipped to `area`, and put that area on the screen
static void paint(gfx_rect_t area) {
    if (area.w <= 0 || area.h <= 0)
        return;

    gfx_set_clip(screen, &area);
    gfx_clear(screen, BG);

    // Status line
    char text[128];
    snprintf(text, sizeof(text), "Drag the box. Arrows nudge it, Esc quits.   mouse %4d,%4d  %c%c%c  events %u",
             mouse.x, mouse.y,
             (buttons & INPUT_BTN_LEFT)   ? 'L' : '-',
             (buttons & INPUT_BTN_MIDDLE) ? 'M' : '-',
             (buttons & INPUT_BTN_RIGHT)  ? 'R' : '-',
             (unsigned)events_seen);
    gfx_fill_rect(screen, 0, 0, screen_w, STATUS_H, GFX_RGB(20, 60, 90));
    gfx_draw_text(screen, 10, (STATUS_H - gfx_line_height(screen)) / 2, text, TEXT);

    // Box, with a translucent shadow
    gfx_set_blend(screen, GFX_BLEND_ALPHA);
    gfx_fill_rect(screen, box.x + SHADOW_OFF, box.y + SHADOW_OFF, BOX_W, BOX_H, SHADOW);
    gfx_set_blend(screen, GFX_BLEND_COPY);

    gfx_color_t fill = dragging ? BOX_DRAG : inside_box(mouse) ? BOX_HOVER : BOX_FILL;
    gfx_fill_rect(screen, box.x, box.y, BOX_W, BOX_H, fill);
    gfx_draw_rect(screen, box.x, box.y, BOX_W, BOX_H, BOX_EDGE);
    if (dragging)
        gfx_draw_rect(screen, box.x + 2, box.y + 2, BOX_W - 4, BOX_H - 4, BOX_EDGE);
    const char* label = dragging ? "dragging" : "drag me";
    gfx_draw_text(screen, box.x + (BOX_W - gfx_text_width(screen, label)) / 2,
                  box.y + (BOX_H - gfx_line_height(screen)) / 2, label, BOX_EDGE);

    draw_cursor();

    gfx_present_rect(screen, &area, area.x, area.y);
}

static gfx_rect_t rect_union(gfx_rect_t a, gfx_rect_t b) {
    int32_t x0 = a.x < b.x ? a.x : b.x;
    int32_t y0 = a.y < b.y ? a.y : b.y;
    int32_t x1 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
    int32_t y1 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
    return gfx_rect(x0, y0, x1 - x0, y1 - y0);
}

static bool rect_overlap(gfx_rect_t a, gfx_rect_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool handle_key(const input_event_t* ev) {
    int32_t step = (ev->mods & INPUT_MOD_SHIFT) ? 40 : 10;
    switch (ev->key) {
        case KEY_ESCAPE: case 'q': case 'Q':
            return false;
        case KEY_LEFT:  box.x -= step; break;
        case KEY_RIGHT: box.x += step; break;
        case KEY_UP:    box.y -= step; break;
        case KEY_DOWN:  box.y += step; break;
    }
    clamp_box();
    return true;
}

static void handle_mouse(const input_event_t* ev) {
    bool was_down = buttons & INPUT_BTN_LEFT;
    bool down     = ev->buttons & INPUT_BTN_LEFT;

    mouse.x = ev->x;
    mouse.y = ev->y;
    buttons = ev->buttons;

    if (down && !was_down && inside_box(mouse)) {
        dragging = true;
        grab.x = mouse.x - box.x;
        grab.y = mouse.y - box.y;
    } else if (!down) {
        dragging = false;
    }

    if (dragging) {
        box.x = mouse.x - grab.x;
        box.y = mouse.y - grab.y;
        clamp_box();
    }
}

int main(void) {
    sec_mask_t need = SEC_GFX | SEC_GFX_SCREEN | SEC_INPUT;
    if (!sec_has(need)) {
        char have[128];
        sec_format(sec_get(), have, sizeof(have));
        printf("input_demo: needs gfx, gfx_screen and input clearance (has: %s)\n", have);
        return 1;
    }

    gfx_info_t info;
    if (gfx_get_info(&info) < 0 || info.width == 0) {
        perror("input_demo: gfx_get_info");
        return 1;
    }
    screen_w = (int32_t)info.width;
    screen_h = (int32_t)info.height;

    screen = gfx_surface_create(screen_w, screen_h);
    if (screen == NULL) {
        perror("input_demo: gfx_surface_create");
        return 1;
    }

    int in = input_open();
    if (in < 0) {
        perror("input_demo: input_open");
        gfx_surface_destroy(screen);
        return 1;
    }

    box.x = (screen_w - BOX_W) / 2;
    box.y = (screen_h - BOX_H) / 2;
    mouse.x = screen_w / 2;
    mouse.y = screen_h / 2;
    clamp_box();
    paint(gfx_rect(0, 0, screen_w, screen_h));

    input_event_t events[INPUT_READ_MAX];
    bool running = true;
    while (running) {
        if (input_wait(in, IPC_FOREVER) < 0) {
            perror("input_demo: input_wait");
            break;
        }

        gfx_rect_t old_box    = box_rect();
        gfx_rect_t old_cursor = cursor_rect();
        bool       old_drag   = dragging;
        bool       old_hover  = inside_box(mouse);

        // Take everything that is waiting, then draw once
        int n;
        while (running && (n = input_read(in, events, INPUT_READ_MAX)) > 0) {
            for (int i = 0; i < n && running; i++) {
                events_seen++;
                if (events[i].type == INPUT_KEY)
                    running = handle_key(&events[i]);
                else if (events[i].type == INPUT_MOUSE)
                    handle_mouse(&events[i]);
            }
        }
        if (!running)
            break;

        gfx_rect_t new_box    = box_rect();
        gfx_rect_t new_cursor = cursor_rect();

        // Only what changed: where things were, where they are now
        paint(status_rect());
        if (old_box.x != new_box.x || old_box.y != new_box.y) {
            if (rect_overlap(old_box, new_box)) {
                paint(rect_union(old_box, new_box));
            } else {
                paint(old_box);
                paint(new_box);
            }
        } else if (old_drag != dragging || old_hover != inside_box(mouse)) {
            paint(new_box);
        }
        if (old_cursor.x != new_cursor.x || old_cursor.y != new_cursor.y) {
            paint(old_cursor);
            paint(new_cursor);
        }
    }

    input_close(in);            // The window manager takes over and repaints
    gfx_surface_destroy(screen);
    return 0;
}

