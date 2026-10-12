#include <kernel/dwm/windows/message_confirm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/util/string.h>
#include <kernel/malloc.h>
#include <kernel/console/display.h>

// ==========================================
// Layout (client coordinates, 6x8 font)
// ==========================================
#define MC_FONT_W          6
#define MC_FONT_H          8
#define MC_PAD_X           15
#define MC_PAD_TOP         20
#define MC_TEXT_GAP        20
#define MC_PAD_BOTTOM      12
#define MC_BUTTON_W        76
#define MC_BUTTON_H        24
#define MC_BUTTON_GAP      10
#define MC_BUTTON_COUNT    3
#define MC_TITLEBAR_H      20     // Matches dwm_allocate_window for style 0
#define MC_TITLE_RESERVE   50     // Title text inset + close/minimize buttons

#define MC_MIN_W           280
#define MC_MAX_W           480
#define MC_MESSAGE_MAX     ((MC_MAX_W - (MC_PAD_X * 2)) / MC_FONT_W)
#define MC_LABEL_MAX       (MC_BUTTON_W / MC_FONT_W)   // Characters per button

#define MC_KEY_ENTER       0x02
#define MC_KEY_ESCAPE      0x1B

struct ConfirmState {
    DWMConfirmCallback callback;
    void*              user;
    bool               completed;      // Callback already ran
    char               message[MC_MESSAGE_MAX + 1];
    char               labels[MC_BUTTON_COUNT][MC_LABEL_MAX + 1];
};

static void mc_button_rect(struct WindowObject* window, int index, int* x, int* y) {
    int total    = (MC_BUTTON_COUNT * MC_BUTTON_W) + ((MC_BUTTON_COUNT - 1) * MC_BUTTON_GAP);
    int client_h = (int)window->h - (int)window->titlebar_height;

    *x = ((int)window->w - total) / 2 + index * (MC_BUTTON_W + MC_BUTTON_GAP);
    *y = client_h - MC_PAD_BOTTOM - MC_BUTTON_H;
}

// Answer once. From DESTROY the window is already going away, so it is not
// asked to close again.
static void mc_complete(WindowHandle handle, struct ConfirmState* state,
                        DWMConfirmResult result, bool close_window) {
    if (state->completed) return;
    state->completed = true;

    // Queued first: anything the callback opens (an error box, say) ends up
    // above this dialog and keeps focus when it goes
    if (close_window) dwm_window_send_event(handle, DWM_EVENT_CLOSE);

    if (state->callback != NULL) state->callback(result, state->user);
}

static void mc_draw(struct WindowObject* window, const struct ConfirmState* state) {
    dwm_draw_rect_filled(0, 0, window->w, window->h, theme.client.background);

    int text_w = (int)strlen(state->message) * MC_FONT_W;
    int text_x = ((int)window->w - text_w) / 2;
    if (text_x < MC_PAD_X) text_x = MC_PAD_X;
    dwm_draw_text(text_x, MC_PAD_TOP, state->message, theme.client.text);

    for (int i = 0; i < MC_BUTTON_COUNT; i++) {
        int bx, by;
        mc_button_rect(window, i, &bx, &by);

        // The first button is the default (Enter): give it the active border
        uint32_t border = (i == 0) ? theme.button.border_active : theme.button.border;

        dwm_draw_rect_filled(bx, by, MC_BUTTON_W, MC_BUTTON_H, theme.button.fill);
        dwm_draw_rect(bx, by, MC_BUTTON_W, MC_BUTTON_H, border);

        const char* label = state->labels[i];
        int label_w = (int)strlen(label) * MC_FONT_W;
        dwm_draw_text(bx + (MC_BUTTON_W - label_w) / 2, by + (MC_BUTTON_H - MC_FONT_H) / 2,
                      label, theme.button.text);
    }
}

static void callback_message_confirm_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct WindowObject* window = dwm_get_window_by_id(handle);
    struct ConfirmState* state = (struct ConfirmState*)dwm_window_resource_get_by_name(handle, "confirm");
    if (window == NULL || state == NULL) return;

    switch (event) {
        case DWM_EVENT_REDRAW:
            mc_draw(window, state);
            break;

        case DWM_EVENT_MOUSE: {
            if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) break;

            int click_x = (int16_t)(wparam & 0xFFFF);
            int click_y = (int16_t)((wparam >> 16) & 0xFFFF);

            for (int i = 0; i < MC_BUTTON_COUNT; i++) {
                int bx, by;
                mc_button_rect(window, i, &bx, &by);
                if (click_x >= bx && click_x < bx + MC_BUTTON_W &&
                    click_y >= by && click_y < by + MC_BUTTON_H) {
                    mc_complete(handle, state, (DWMConfirmResult)i, true);
                    break;
                }
            }
            break;
        }

        case DWM_EVENT_KEYBOARD: {
            if ((wparam >> 8) & 0x80) break;   // Key release
            uint8_t key = (uint8_t)(wparam & 0xFF);

            if (key == MC_KEY_ENTER)       mc_complete(handle, state, DWM_CONFIRM_YES, true);
            else if (key == MC_KEY_ESCAPE) mc_complete(handle, state, DWM_CONFIRM_CANCEL, true);
            break;
        }

        // Close box, or destroyed by someone else: that is a Cancel.
        // The state is still valid here (resources are freed after DESTROY).
        case DWM_EVENT_DESTROY:
            mc_complete(handle, state, DWM_CONFIRM_CANCEL, false);
            break;

        default:
            break;
    }
}

static void mc_set_label(char* out, const char* label) {
    strncpy(out, label, MC_LABEL_MAX);
    out[MC_LABEL_MAX] = '\0';
}

WindowHandle dwm_summon_message_confirm(const char* title, const char* message,
                                        const char* yes_label, const char* no_label,
                                        DWMConfirmCallback callback, void* user) {
    DWM_GUARD();
    if (title == NULL)   title = "";
    if (message == NULL) message = "";

    struct ConfirmState* state = (struct ConfirmState*)malloc(sizeof(struct ConfirmState));
    if (state == NULL) return 0;

    memset(state, 0, sizeof(struct ConfirmState));
    state->callback = callback;
    state->user     = user;
    strncpy(state->message, message, MC_MESSAGE_MAX);
    mc_set_label(state->labels[DWM_CONFIRM_YES],    yes_label ? yes_label : "Yes");
    mc_set_label(state->labels[DWM_CONFIRM_NO],     no_label  ? no_label  : "No");
    mc_set_label(state->labels[DWM_CONFIRM_CANCEL], "Cancel");

    int display_w = display_get_width();
    int display_h = display_get_height();

    // Wide enough for the message, the buttons and the title
    int width   = (int)strlen(state->message) * MC_FONT_W + (MC_PAD_X * 2);
    int buttons = (MC_BUTTON_COUNT * MC_BUTTON_W) + ((MC_BUTTON_COUNT - 1) * MC_BUTTON_GAP) + (MC_PAD_X * 2);
    int title_w = (int)strnlen(title, DWM_MAX_TITLE_LEN) * MC_FONT_W + MC_TITLE_RESERVE;
    if (width < buttons)  width = buttons;
    if (width < title_w)  width = title_w;
    if (width < MC_MIN_W) width = MC_MIN_W;
    if (width > MC_MAX_W) width = MC_MAX_W;
    if (width > display_w - 40) width = display_w - 40;

    int height = MC_TITLEBAR_H + MC_PAD_TOP + MC_FONT_H + MC_TEXT_GAP + MC_BUTTON_H + MC_PAD_BOTTOM;

    int x = (display_w - width) / 2;
    int y = (display_h - taskbar.height - height) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    WindowClass wclass;
    memset(&wclass, 0, sizeof(WindowClass));
    wclass.x          = (uint16_t)x;
    wclass.y          = (uint16_t)y;
    wclass.width      = (uint16_t)width;
    wclass.height     = (uint16_t)height;
    wclass.max_width  = (uint16_t)width;
    wclass.max_height = (uint16_t)height;
    strncpy(wclass.title, title, DWM_MAX_TITLE_LEN - 1);

    struct WindowObject* window = dwm_allocate_window(wclass, 0, callback_message_confirm_handler);
    if (window == NULL) {
        free(state);
        return 0;
    }

    // From here the window owns `state` (freed by dwm_window_resource_free_all)
    if (!dwm_window_resource_add(window->id, "confirm", state)) {
        free(state);
        dwm_destroy_window(window->id);
        return 0;
    }

    dwm_set_focus(window);
    return window->id;
}
