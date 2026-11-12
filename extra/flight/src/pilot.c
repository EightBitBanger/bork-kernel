/*
 * pilot.c: the pilots. The first screen lists them, with "New pilot" at the
 * end: Up / Down picks, Enter (or a double click, or SELECT) goes on to the
 * theater and aircraft menu as that pilot. On "New pilot", type a name and
 * press Enter (Escape gives up). Delete, pressed twice, removes the one
 * picked.
 *
 * A crash, or being shot down, kills the pilot: the screen goes black for a
 * few seconds (main.c), then the pilot stays on the list with a tombstone by
 * the name and can't fly again, only be removed. Ejecting is surviving.
 *
 * A new pilot starts as a Second Lieutenant with no score. What a flight
 * destroys counts toward its sortie score (SCORE_* in flight.h; anything
 * friendly counts against it), shown on the HUD. It goes to the pilot only
 * when the flight ends well: down on the ground, not crashed, and Escape
 * pressed. Crash, get shot down or eject and it is lost. Then this screen
 * says how the flight went, and whether it brought a promotion. The rank
 * follows the score (ranks below).
 *
 * The pilots are kept in the registry (PILOT_KEY in flight.h), saved each
 * time one is added, removed, scores or dies. A dead pilot's line has a
 * fourth field, "KIA"; lines without it (from before) are living pilots.
 */

#include <errno.h>
#include <os/input.h>
#include <os/registry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flight.h"

#define PANEL_W         460
#define PANEL_H         360

#define C_PICK          GFX_RGB(150, 190, 240)
#define C_PICK_IDLE     GFX_RGB(200, 204, 210)
#define C_BUTTON        GFX_RGB(120, 200, 130)
#define C_BUTTON_HOVER  GFX_RGB(150, 230, 160)
#define C_PANEL_EDGE    GFX_RGB(40, 52, 70)
#define C_GOOD          GFX_RGB(20, 110, 40)
#define C_BUTTON_OFF    GFX_RGB(180, 184, 190)
#define C_STONE         GFX_RGB(120, 124, 132)
#define C_STONE_EDGE    GFX_RGB(60, 64, 72)
#define C_DEAD_TEXT     GFX_RGB(90, 94, 100)

typedef struct {
    int         score;              // From here up
    const char* name;
    const char* abbr;
} rank_t;

static const rank_t ranks[] = {
    {     0, "Second Lieutenant",  "2nd Lt."    },
    {   500, "First Lieutenant",   "1st Lt."    },
    {  1500, "Captain",            "Capt."      },
    {  3000, "Major",              "Maj."       },
    {  5000, "Lieutenant Colonel", "Lt. Col."   },
    {  8000, "Colonel",            "Col."       },
    { 12000, "Brigadier General",  "Brig. Gen." },
    { 17000, "Major General",      "Maj. Gen."  },
    { 23000, "Lieutenant General", "Lt. Gen."   },
    { 30000, "General",            "Gen."       },
};

typedef struct {
    char name[PILOT_NAME_MAX + 1];
    int  score;
    bool dead;                      // Crashed: on the list, but can't fly
} pilot_t;

int  sortie_score;
bool pilot_screen = true;

static pilot_t  pilots[MAX_PILOTS];
static int      pilot_count;
static int      current = -1;       // The one flying (-1: none yet)
static int      sel;                // 0 .. pilot_count (that one: "New pilot")
static int      first_row;
static bool     typing;             // Naming a new pilot
static char     typed[PILOT_NAME_MAX + 1];
static int      delete_armed = -1;  // Delete once more removes this one
static char     note[128];          // How the last flight went, or what went wrong
static bool     note_good;

static rect_t   rows[MAX_PILOTS + 1], select_button, delete_button;
static int      rows_n;
static bool     select_hover;

static int rank_of(int score) {
    int r = 0;
    while (r + 1 < (int)COUNT(ranks) && score >= ranks[r + 1].score)
        r++;
    return r;
}

// 12345 as "12,345"
static const char* points(int n) {
    static char bufs[2][24];
    static int which;
    char* out = bufs[which ^= 1];
    char digits[16];
    snprintf(digits, sizeof(digits), "%d", n < 0 ? -n : n);
    size_t len = strlen(digits), o = 0;
    if (n < 0)
        out[o++] = '-';
    for (size_t i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
    return out;
}

//
// Keeping them
//

static void say(bool good, const char* fmt, const char* a, const char* b) {
    snprintf(note, sizeof(note), fmt, a, b);
    note_good = good;
}

static void save(void) {
    char text[OS_REG_VALUE_MAX];
    size_t n = 0;
    for (int i = 0; i < pilot_count && n < sizeof(text); i++)
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s\t%d\t%s%s\n", pilots[i].name, pilots[i].score,
                              ranks[rank_of(pilots[i].score)].name, pilots[i].dead ? "\tKIA" : "");
    if (n >= sizeof(text))
        n = sizeof(text) - 1;
    text[n] = 0;
    if (os_reg_set_string(PILOT_KEY, "pilots", text) < 0 && errno != EIO)
        say(false, "Couldn't save the pilots: %s%s", strerror(errno), "");
    if (current >= 0)
        os_reg_set_string(PILOT_KEY, "last", pilots[current].name);
}

void pilot_init(void) {
    char text[OS_REG_VALUE_MAX], last[PILOT_NAME_MAX + 1] = "";
    pilot_count = 0;
    if (os_reg_get_string(PILOT_KEY, "pilots", text, sizeof(text)) == 0) {
        for (char* line = text; *line && pilot_count < MAX_PILOTS;) {
            char* end = strchr(line, '\n');
            if (end)
                *end = 0;
            char* tab = strchr(line, '\t');
            if (tab) {
                *tab = 0;
                pilot_t* p = &pilots[pilot_count];
                snprintf(p->name, sizeof(p->name), "%s", line);
                p->score = atoi(tab + 1);
                if (p->score < 0)
                    p->score = 0;
                // name, score, rank, then "KIA" for a dead one (older lines stop at the rank)
                char* rank = strchr(tab + 1, '\t');
                char* fate = rank ? strchr(rank + 1, '\t') : NULL;
                p->dead = fate && !strncmp(fate + 1, "KIA", 3);
                if (p->name[0])
                    pilot_count++;
            }
            if (!end)
                break;
            line = end + 1;
        }
    }
    os_reg_get_string(PILOT_KEY, "last", last, sizeof(last));
    sel = 0;
    for (int i = 0; i < pilot_count; i++)
        if (!strcmp(pilots[i].name, last))
            sel = i;
    typing = pilot_count == 0;      // None yet: straight to naming one
    typed[0] = 0;
}

//
// The one flying, and the score
//

const char* pilot_title(void) {
    static char buf[PILOT_NAME_MAX + 16];
    if (current < 0)
        return "";
    snprintf(buf, sizeof(buf), "%s %s", ranks[rank_of(pilots[current].score)].abbr, pilots[current].name);
    return buf;
}

int pilot_score(void) {
    return current >= 0 ? pilots[current].score : 0;
}

void sortie_start(void) {
    sortie_score = 0;
    note[0] = 0;
}

void sortie_add(int pts) {
    sortie_score += pts;
}

void sortie_end(bool safe) {
    note[0] = 0;
    if (current < 0)
        return;
    pilot_t* p = &pilots[current];
    if (p->dead) {
        say(false, "%s %s was killed in the crash", ranks[rank_of(p->score)].name, p->name);
        sortie_score = 0;
        current = -1;
        return;
    }
    if (!safe) {
        if (sortie_score > 0)
            say(false, "Aircraft lost: the %s points from that flight are gone", points(sortie_score), "");
        else
            say(false, "Aircraft lost%s%s", "", "");
        sortie_score = 0;
        return;
    }
    int was = rank_of(p->score);
    p->score += sortie_score;
    if (p->score < 0)
        p->score = 0;
    int now = rank_of(p->score);
    char got[24];
    snprintf(got, sizeof(got), "%s%s", sortie_score > 0 ? "+" : "", points(sortie_score));
    if (now > was)
        say(true, "Back safe, %s points. Promoted to %s!", got, ranks[now].name);
    else if (now < was)
        say(false, "Back safe, %s points. Demoted to %s", got, ranks[now].name);
    else
        say(true, "Back safe, %s points%s", got, "");
    sortie_score = 0;
    save();
}

// Crashed or shot down: the pilot is gone, kept at once so quitting now changes nothing
void pilot_killed(void) {
    if (current < 0 || pilots[current].dead)
        return;
    pilots[current].dead = true;
    sortie_score = 0;
    save();
}

//
// The screen
//

static bool choose(void) {
    if (sel >= pilot_count)
        return false;
    if (pilots[sel].dead) {
        say(false, "%s is no longer with us%s", pilots[sel].name, "");
        return false;
    }
    current = sel;
    delete_armed = -1;
    note[0] = 0;
    os_reg_set_string(PILOT_KEY, "last", pilots[current].name);
    return true;
}

static void add_typed(void) {
    char name[PILOT_NAME_MAX + 1];
    size_t a = 0, b = strlen(typed);
    while (a < b && typed[a] == ' ')
        a++;
    while (b > a && typed[b - 1] == ' ')
        b--;
    snprintf(name, sizeof(name), "%.*s", (int)(b - a), typed + a);
    if (!name[0])
        return;
    for (int i = 0; i < pilot_count; i++)
        if (!strcasecmp(pilots[i].name, name)) {
            say(false, "There is a pilot called %s already%s", name, "");
            return;
        }
    if (pilot_count >= MAX_PILOTS) {
        say(false, "That's the most pilots there can be: remove one first%s%s", "", "");
        return;
    }
    pilot_t* p = &pilots[pilot_count];
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->score = 0;
    p->dead = false;
    sel = pilot_count++;
    typing = false;
    typed[0] = 0;
    say(true, "%s %s joins the squadron", ranks[0].name, p->name);
    save();
}

static void remove_selected(void) {
    if (sel >= pilot_count)
        return;
    if (delete_armed != sel) {
        delete_armed = sel;
        say(false, "Press Delete again to remove %s%s", pilots[sel].name, "");
        return;
    }
    say(true, "%s removed%s", pilots[sel].name, "");
    if (current == sel)
        current = -1;
    else if (current > sel)
        current--;
    memmove(&pilots[sel], &pilots[sel + 1], sizeof(pilot_t) * (size_t)(pilot_count - sel - 1));
    pilot_count--;
    delete_armed = -1;
    save();
}

bool pilot_key(uint16_t key) {
    if (key == '\t')
        key = KEY_TAB;
    if (typing) {
        size_t n = strlen(typed);
        switch (key) {
            case KEY_ENTER: case '\r': case '\n':
                add_typed();
                return false;
            case KEY_ESCAPE:
                typing = false;
                typed[0] = 0;
                return false;
            case KEY_BACKSPACE: case '\b': case 0x7F:
                if (n)
                    typed[n - 1] = 0;
                return false;
        }
        if (key >= 0x20 && key < 0x7F && n < PILOT_NAME_MAX && (n || key != ' ')) {
            typed[n] = (char)key;
            typed[n + 1] = 0;
        }
        return false;
    }
    if (key != KEY_DELETE)
        delete_armed = -1;
    switch (key) {
        case KEY_UP:   sel = (sel + pilot_count) % (pilot_count + 1); return false;
        case KEY_DOWN: sel = (sel + 1) % (pilot_count + 1);           return false;
        case KEY_DELETE:
            remove_selected();
            return false;
        case KEY_ENTER: case '\r': case '\n': case ' ':
            if (sel >= pilot_count) {
                typing = true;
                typed[0] = 0;
                note[0] = 0;
                return false;
            }
            return choose();
    }
    return false;
}

bool pilot_click(int32_t x, int32_t y, int clicks) {
    if (menu_in_rect(&select_button, x, y)) {
        if (typing) {
            add_typed();
            return false;
        }
        return pilot_key(KEY_ENTER);
    }
    if (!typing && menu_in_rect(&delete_button, x, y)) {
        remove_selected();
        return false;
    }
    for (int i = 0; i < rows_n; i++) {
        if (!menu_in_rect(&rows[i], x, y))
            continue;
        int was = sel;
        sel = first_row + i;
        if (sel != was)
            delete_armed = -1;
        if (typing && sel < pilot_count)
            typing = false;             // Picked a pilot instead
        if (sel >= pilot_count && !typing) {
            typing = true;
            typed[0] = 0;
            note[0] = 0;
            return false;
        }
        return clicks >= 2 && choose();
    }
    return false;
}

void pilot_move(int32_t x, int32_t y) {
    select_hover = menu_in_rect(&select_button, x, y);
}

// A tiny gravestone, 9 x 11: rounded top, a cross, on a strip of ground
static void tombstone(int32_t x, int32_t y) {
    fill_rect(x + 2, y, 5, 1, C_STONE_EDGE, 1);
    fill_rect(x + 1, y + 1, 7, 1, C_STONE_EDGE, 1);
    fill_rect(x + 2, y + 1, 5, 1, C_STONE, 1);
    fill_rect(x + 1, y + 2, 7, 8, C_STONE_EDGE, 1);
    fill_rect(x + 2, y + 2, 5, 8, C_STONE, 1);
    fill_rect(x + 4, y + 3, 1, 5, C_STONE_EDGE, 1);     // The cross
    fill_rect(x + 3, y + 4, 3, 1, C_STONE_EDGE, 1);
    fill_rect(x, y + 10, 9, 1, C_GOOD, 1);              // Grass
}

static const char* row_name(int i) {
    static char buf[PILOT_NAME_MAX + 40];
    if (i < pilot_count)
        snprintf(buf, sizeof(buf), "%s%s %s", pilots[i].dead ? "  " : "", ranks[rank_of(pilots[i].score)].abbr,
                 pilots[i].name);
    else if (typing)
        snprintf(buf, sizeof(buf), "Name: %s_", typed);
    else
        snprintf(buf, sizeof(buf), "+ New pilot");
    return buf;
}

void pilot_draw(void) {
    int32_t lh = gfx_line_height(surf), cw = gfx_char_width(surf);
    int32_t want_h = 14 + lh * 2 + lh + 8 + (pilot_count + 1) * (lh + 4) + lh * 6 + 32;
    if (want_h > PANEL_H)
        want_h = PANEL_H;
    if (want_h < lh * 16)
        want_h = lh * 16;
    int32_t w = sw - 32 < PANEL_W ? sw - 32 : PANEL_W, h = sh - 24 < want_h ? sh - 24 : want_h;
    int32_t x = (sw - w) / 2, y = (sh - h) / 2;
    menu_panel(x, y, w, h);

    text_at(sw / 2, y + 8, 0, C_TEXT, "F L I G H T");

    // The list, down to the details
    int32_t top = y + 14 + lh * 2, details = y + h - lh * 6 - 24;
    int nrows = (int)((details - top - lh - 8) / (lh + 4));
    if (sel > pilot_count)
        sel = pilot_count;
    if (typing)
        sel = pilot_count;
    rows_n = menu_list(x + 12, top, w - 24, nrows, "PILOT", pilot_count + 1, sel, &first_row, true, row_name, rows);
    for (int i = 0; i < rows_n; i++) {
        int k = first_row + i;
        if (k < pilot_count)
            text_at(rows[i].x + rows[i].w - 8, rows[i].y + 2, 1, C_TEXT, "%s", points(pilots[k].score));
        if (k < pilot_count && pilots[k].dead)
            tombstone(rows[i].x + 6 + (cw * 2 - 9) / 2, rows[i].y + (rows[i].h - 11) / 2);
    }

    // About the one picked
    int32_t dx = x + 12, dy = details;
    fill_rect(x + 12, dy - 4, w - 24, 1, C_PANEL_EDGE, 1);
    if (typing) {
        text_at(dx, dy, -1, C_TEXT, "Type the new pilot's name, then Enter (Esc: cancel)");
        text_at(dx, dy + lh, -1, C_TEXT, "New pilots start as %s", ranks[0].name);
    } else if (sel < pilot_count) {
        const pilot_t* p = &pilots[sel];
        int r = rank_of(p->score);
        text_at(dx, dy, -1, C_TEXT, "%s %s", ranks[r].name, p->name);
        if (p->dead)
            text_at(dx, dy + lh, -1, C_DEAD_TEXT, "Killed in action with %s points. Can't fly; Delete removes", points(p->score));
        else if (r + 1 < (int)COUNT(ranks))
            text_at(dx, dy + lh, -1, C_TEXT, "%s points, %s more to %s", points(p->score),
                    points(ranks[r + 1].score - p->score), ranks[r + 1].name);
        else
            text_at(dx, dy + lh, -1, C_TEXT, "%s points, the top rank", points(p->score));
    } else {
        text_at(dx, dy, -1, C_TEXT, "Enter to sign up a new pilot");
    }
    if (note[0])
        text_at(dx, dy + lh * 2 + 4, -1, note_good ? C_GOOD : C_TEXT_ALERT, "%s", note);

    // SELECT and DELETE, and the keys
    int32_t bw = cw * 14, bh = lh + 8;
    select_button = (rect_t){ x + w - 12 - bw, y + h - bh - 8, bw, bh };
    bool can_fly = typing || sel >= pilot_count || !pilots[sel].dead;
    fill_rect(select_button.x, select_button.y, bw, bh, !can_fly ? C_BUTTON_OFF : select_hover ? C_BUTTON_HOVER : C_BUTTON, 1);
    draw_rect(select_button.x, select_button.y, bw - 1, bh - 1, C_PANEL_EDGE);
    text_at(select_button.x + bw / 2, select_button.y + 6, 0, can_fly ? C_TEXT : C_DEAD_TEXT, typing ? "ADD" : "SELECT");
    if (!typing && sel < pilot_count) {
        delete_button = (rect_t){ select_button.x - 8 - bw, select_button.y, bw, bh };
        fill_rect(delete_button.x, delete_button.y, bw, bh, delete_armed == sel ? C_PICK : C_PICK_IDLE, 1);
        draw_rect(delete_button.x, delete_button.y, bw - 1, bh - 1, C_PANEL_EDGE);
        text_at(delete_button.x + bw / 2, delete_button.y + 6, 0, C_TEXT, "REMOVE");
    } else {
        delete_button = (rect_t){ 0, 0, 0, 0 };
    }
    text_at(x + 12, y + h - lh - 10, -1, C_TEXT, "Arrows: choose  Ctrl+Q: quit");
}
