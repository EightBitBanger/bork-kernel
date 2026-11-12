/*
 * Building: the saved document goes through the compiler (tcc unless the
 * settings name another), and Run starts what it made.
 *
 *     <compiler> [-Wall] <options> -o <output> <file> <libraries>
 *
 * runs in the document's folder on a terminal (spawn_tty), and what it
 * prints shows in the output pane under the text. The kernel tells nobody
 * a program's exit status, so a build counts as good when the output file
 * is there afterwards and nothing said "error:". The old output is deleted
 * first, so a failed build can't leave a stale one that looks new.
 *
 * Run builds first, then starts the output: a windowed program as the
 * desktop would, beside the editor; a console program on a terminal of
 * cedit's own, its output in the pane. Stop (Escape over the pane, or the
 * Build menu) closes that terminal: the program reads end of file, which
 * ends most programs.
 *
 * A message with a place in it ("hello.c:12: error: ...") is a link: click
 * it, or F8 / Shift+F8 to step through them, to go to that line.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/os.h>
#include <os/ipc.h>
#include <os/process.h>
#include <os/security.h>
#include "cedit.h"

#define OUT_LINES       300         // Kept in the pane; the oldest go first
#define OUT_COLS        200         // A longer line is cut
#define PANE_ROWS       8
#define PANE_HEAD_H     16
#define ARGS_MAX        48
#define LAUNCH_MASK     (SEC_FILE | SEC_CONSOLE | SEC_GFX | SEC_IPC | SEC_EXEC)

typedef enum { OUT_TEXT, OUT_INFO, OUT_GOOD, OUT_WARNING, OUT_ERROR } out_kind_t;

static struct {
    char     text[OUT_LINES][OUT_COLS];
    uint8_t  kind[OUT_LINES];
    uint32_t start, count;          // Ring: line k is text[(start + k) % OUT_LINES]
    char     partial[OUT_COLS];     // Written, but no '\n' yet
    uint32_t partial_len;
    uint32_t top;                   // First line shown
    bool     scrolled;              // Scrolled up: else the last line stays in view
    bool     visible;
    int32_t  shown_rows;            // From the last paint
} out;

static int32_t picked = -1;           // Picked line (F8 / click), or -1

typedef enum { JOB_IDLE, JOB_COMPILING, JOB_RUNNING } job_state_t;

static struct {
    job_state_t  state;
    build_goal_t goal;
    int          tty;
    char         dir[PATH_MAX];     // The document's folder: where it all runs
    char         output[PATH_MAX];  // What the compiler makes
    char         name[64];          // Shown: the source's or the program's name
    uint32_t     errors, warnings;
    uint64_t     started_ms;
} job = { .tty = -1 };

//
// The pane's lines
//

static uint32_t total_lines(void) { return out.count + (out.partial_len > 0 ? 1 : 0); }

static const char* line_text(uint32_t k) {
    if (k == out.count)
        return out.partial;
    return out.text[(out.start + k) % OUT_LINES];
}

static out_kind_t line_kind(uint32_t k) {
    if (k == out.count)
        return OUT_TEXT;
    return (out_kind_t)out.kind[(out.start + k) % OUT_LINES];
}

static out_kind_t classify(const char* s) {
    if (job.state != JOB_COMPILING)
        return OUT_TEXT;
    if (strstr(s, "error:") != NULL)   return OUT_ERROR;
    if (strstr(s, "warning:") != NULL) return OUT_WARNING;
    return OUT_TEXT;
}

static void add_line(const char* s, out_kind_t kind) {
    uint32_t slot;
    if (out.count < OUT_LINES) {
        slot = (out.start + out.count++) % OUT_LINES;
    } else {
        slot = out.start;           // Full: the oldest makes room
        out.start = (out.start + 1) % OUT_LINES;
        if (out.top > 0) out.top--;
        if (picked >= 0) picked--;
    }
    strlcpy(out.text[slot], s, OUT_COLS);
    out.kind[slot] = (uint8_t)kind;

    if (kind == OUT_ERROR) job.errors++;
    if (kind == OUT_WARNING) job.warnings++;
    editor_redraw();
}

static void add_info(out_kind_t kind, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_info(out_kind_t kind, const char* fmt, ...) {
    char buf[OUT_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    add_line(buf, kind);
}

static void flush_partial(void) {
    if (out.partial_len == 0)
        return;
    out.partial[out.partial_len] = '\0';
    out.partial_len = 0;
    add_line(out.partial, classify(out.partial));
    out.partial[0] = '\0';
}

// Bytes from the terminal: lines into the pane, tabs to spaces, control
// codes dropped
static void take_output(const char* buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n') {
            if (out.partial_len == 0) {
                add_line("", OUT_TEXT);
            } else {
                flush_partial();
            }
            continue;
        }
        if (c == '\t') {
            do {
                if (out.partial_len < OUT_COLS - 1) out.partial[out.partial_len++] = ' ';
            } while (out.partial_len % 8 != 0 && out.partial_len < OUT_COLS - 1);
        } else if (c >= 32 && c != 127) {
            if (out.partial_len < OUT_COLS - 1) out.partial[out.partial_len++] = (char)c;
        }
    }
    out.partial[out.partial_len] = '\0';
    editor_redraw();
}

void pane_clear(void) {
    out.start = out.count = 0;
    out.partial_len = 0;
    out.partial[0] = '\0';
    out.top = 0;
    picked = -1;
    out.scrolled = false;
    editor_redraw();
}

//
// "file.c:12: error: ..." -> file.c, 12
//

static bool parse_place(const char* s, char* file, size_t size, uint32_t* line) {
    const char* p = s;
    static const char prefix[] = "In file included from ";
    if (strncmp(p, prefix, sizeof(prefix) - 1) == 0)
        p += sizeof(prefix) - 1;
    while (*p == ' ')
        p++;

    for (const char* c = strchr(p, ':'); c != NULL; c = strchr(c + 1, ':')) {
        const char* d = c + 1;
        uint32_t n = 0;
        int digits = 0;
        while (*d >= '0' && *d <= '9' && digits < 9) {
            n = n * 10 + (uint32_t)(*d - '0');
            d++;
            digits++;
        }
        if (digits > 0 && (*d == ':' || *d == ',') && c > p && n > 0) {
            size_t len = (size_t)(c - p);
            if (len >= size)
                return false;
            memcpy(file, p, len);
            file[len] = '\0';
            *line = n;
            return true;
        }
    }
    return false;
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Go to the place line k names. False when it names none.
static bool jump_to(uint32_t k) {
    char file[PATH_MAX];
    uint32_t line;
    if (!parse_place(line_text(k), file, sizeof(file), &line))
        return false;
    const char* doc = doc_path();
    if (doc[0] != '\0' && strcmp(base_name(file), base_name(doc)) == 0) {
        doc_goto_line(line, 1);
        if (main_win != 0)
            wm_window_focus(main_win);
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "Line %u of %s, which isn't open", (unsigned)line, base_name(file));
        wm_notify("Not in this file", msg);
    }
    return true;
}

static void reveal_line(uint32_t k) {
    uint32_t rows = out.shown_rows > 0 ? (uint32_t)out.shown_rows : PANE_ROWS;
    if (k < out.top)
        out.top = k;
    else if (k >= out.top + rows)
        out.top = k - rows + 1;
    out.scrolled = true;
}

void build_next_error(int dir) {
    uint32_t n = out.count;
    if (n == 0)
        return;
    if (!out.visible)
        pane_toggle();
    int32_t k = picked;
    for (uint32_t tries = 0; tries < n; tries++) {
        k += dir;
        if (k < 0) k = (int32_t)n - 1;
        if (k >= (int32_t)n) k = 0;
        out_kind_t kind = line_kind((uint32_t)k);
        if (kind != OUT_ERROR && kind != OUT_WARNING)
            continue;
        char file[PATH_MAX];
        uint32_t line;
        if (!parse_place(line_text((uint32_t)k), file, sizeof(file), &line))
            continue;
        picked = k;
        reveal_line((uint32_t)k);
        jump_to((uint32_t)k);
        editor_redraw();
        return;
    }
    wm_notify("Output", "No errors or warnings with a line to go to");
}

//
// The pane
//

bool pane_visible(void) { return out.visible; }

void pane_toggle(void) {
    out.visible = !out.visible;
    editor_redraw();
}

int32_t pane_height(int32_t client_h, int32_t row_h) {
    if (!out.visible)
        return 0;
    int32_t h = PANE_HEAD_H + PANE_ROWS * row_h + 4;
    int32_t most = client_h / 3;
    if (most < PANE_HEAD_H + 2 * row_h)
        most = PANE_HEAD_H + 2 * row_h;
    return h < most ? h : most;
}

static uint32_t max_top(void) {
    uint32_t total = total_lines();
    uint32_t rows = out.shown_rows > 0 ? (uint32_t)out.shown_rows : PANE_ROWS;
    return total > rows ? total - rows : 0;
}

static gfx_color_t kind_color(out_kind_t kind) {
    // The text area's background is light or dark: pick what reads on it
    gfx_color_t bg = theme.edit.background;
    uint32_t lum = ((bg >> 16) & 0xFF) * 3 + ((bg >> 8) & 0xFF) * 6 + (bg & 0xFF);
    bool dark = lum < 1280;
    switch (kind) {
        case OUT_INFO:    return theme.client.text_soft;
        case OUT_GOOD:    return dark ? GFX_RGB(120, 220, 120) : GFX_RGB(0, 120, 0);
        case OUT_WARNING: return dark ? GFX_RGB(240, 190, 90) : GFX_RGB(170, 100, 0);
        case OUT_ERROR:   return dark ? GFX_RGB(255, 120, 110) : GFX_RGB(190, 20, 20);
        default:          return theme.edit.text;
    }
}

void pane_paint(gfx_surface_t* s, const gfx_rect_t* r, int32_t cw, int32_t row_h) {
    (void)cw;
    // Header: "Output", what is going on, and an X to hide it
    gfx_fill_rect(s, r->x, r->y, r->w, PANE_HEAD_H, theme.button.fill);
    gfx_draw_hline(s, r->x, r->y, r->w, theme.button.border);
    int32_t ty = gfx_text_y(s, r->y, PANE_HEAD_H);
    const char* what = job.state == JOB_COMPILING ? "Output - compiling..."
                     : job.state == JOB_RUNNING   ? "Output - running (Esc stops)"
                     : "Output";
    gfx_draw_text(s, r->x + 8, ty, what, theme.button.text);
    gfx_draw_text(s, r->x + r->w - 14, ty, "x", theme.button.text);

    gfx_rect_t body = gfx_rect(r->x, r->y + PANE_HEAD_H, r->w, r->h - PANE_HEAD_H);
    gfx_fill_rect(s, body.x, body.y, body.w, body.h, theme.edit.background);
    gfx_draw_hline(s, r->x, body.y, r->w, theme.button.border);

    out.shown_rows = (body.h - 4) / row_h;
    if (out.shown_rows < 1) out.shown_rows = 1;
    if (!out.scrolled)
        out.top = max_top();
    if (out.top > max_top())
        out.top = max_top();

    gfx_save(s);
    gfx_set_clip(s, &body);
    uint32_t total = total_lines();
    for (int32_t row = 0; row < out.shown_rows; row++) {
        uint32_t k = out.top + (uint32_t)row;
        if (k >= total)
            break;
        int32_t y = body.y + 2 + row * row_h;
        if ((int32_t)k == picked)
            gfx_fill_rect(s, body.x, y, body.w, row_h, theme.edit.selection);
        gfx_draw_text(s, body.x + 8, gfx_text_y(s, y, row_h), line_text(k), kind_color(line_kind(k)));
    }
    gfx_restore(s);
}

void pane_mouse_down(const gfx_rect_t* r, int32_t x, int32_t y, int32_t row_h) {
    if (y < r->y + PANE_HEAD_H) {
        if (x >= r->x + r->w - 20)
            pane_toggle();          // The X
        return;
    }
    int32_t row = (y - (r->y + PANE_HEAD_H + 2)) / row_h;
    if (row < 0)
        return;
    uint32_t k = out.top + (uint32_t)row;
    if (k >= out.count)
        return;
    picked = (int32_t)k;
    out.scrolled = true;
    jump_to(k);
    editor_redraw();
}

void pane_wheel(int notches) {
    int64_t t = (int64_t)out.top + notches * 3;
    if (t < 0) t = 0;
    if (t > (int64_t)max_top()) t = max_top();
    out.top = (uint32_t)t;
    out.scrolled = (out.top != max_top());
    editor_redraw();
}

//
// Starting things
//

bool build_busy(void)  { return job.state != JOB_IDLE; }
int  build_handle(void) { return job.tty; }

// Split `text` at blanks into argv (quotes keep blanks: "a b" 'a b')
static int split_args(char* text, char** argv, int at, int max) {
    char* p = text;
    while (*p != '\0' && at < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        char* dst = p;
        argv[at++] = p;
        char quote = 0;
        while (*p != '\0') {
            if (quote) {
                if (*p == quote) { quote = 0; p++; continue; }
            } else if (*p == '"' || *p == '\'') {
                quote = *p++;
                continue;
            } else if (*p == ' ' || *p == '\t') {
                p++;
                break;
            }
            *dst++ = *p++;
        }
        if (dst < p) *dst = '\0';
        else if (*p != '\0') *p = '\0';
    }
    return at;
}

static bool on_terminal(const char* path, char** argv) {
    int ends[2];
    if (ipc_pipe(ends) < 0)
        return false;
    pid_t pid = spawn_tty(path, argv, job.dir, sec_get() & LAUNCH_MASK, ends[1]);
    int err = errno;
    ipc_close(ends[1]);             // The child has its own
    if (pid < 0) {
        ipc_close(ends[0]);
        errno = err;
        return false;
    }
    job.tty = ends[0];
    return true;
}

static void run_output(void) {
    char* argv[2] = { job.output, NULL };
    strlcpy(job.name, base_name(job.output), sizeof(job.name));

    if (program_kind(job.output) == PROGRAM_WINDOWED) {
        if (spawn_ex(job.output, argv, job.dir, sec_get() & LAUNCH_MASK) < 0)
            add_info(OUT_ERROR, "Can't start %s: %s", job.name, strerror(errno));
        else
            add_info(OUT_INFO, "Started %s in its own window", job.name);
        return;
    }

    if (!on_terminal(job.output, argv)) {
        add_info(OUT_ERROR, "Can't start %s: %s", job.name, strerror(errno));
        return;
    }
    job.state = JOB_RUNNING;
    job.started_ms = os_time_ms();
    add_info(OUT_INFO, "Running %s", job.name);
}

void build_start(build_goal_t goal) {
    if (job.state != JOB_IDLE) {
        wm_notify("Build", job.state == JOB_RUNNING ? "The program is still running: stop it first (Esc)"
                                                    : "Already compiling");
        return;
    }
    const char* src = doc_path();
    if (src[0] == '\0')
        return;

    out.visible = true;
    pane_clear();
    job.errors = job.warnings = 0;
    job.goal = goal;

    // The folder, and the output beside the source
    strlcpy(job.dir, src, sizeof(job.dir));
    char* slash = strrchr(job.dir, '/');
    if (slash == job.dir) slash[1] = '\0';
    else if (slash) *slash = '\0';
    const char* file = base_name(src);

    char name[PATH_MAX];
    if (cfg.cc_output[0] != '\0') {
        strlcpy(name, cfg.cc_output, sizeof(name));
    } else {
        strlcpy(name, file, sizeof(name));
        char* dot = strrchr(name, '.');
        if (dot != NULL && dot != name && strcmp(dot, ".c") == 0)
            *dot = '\0';
        else
            strlcat(name, ".out", sizeof(name));    // Never the source itself
    }
    if (name[0] == '/')
        strlcpy(job.output, name, sizeof(job.output));
    else
        snprintf(job.output, sizeof(job.output), "%s%s%s", job.dir,
                 job.dir[strlen(job.dir) - 1] == '/' ? "" : "/", name);
    if (strcmp(job.output, src) == 0) {
        add_info(OUT_ERROR, "The output name is the source's: change it in Settings > Compiler");
        return;
    }

    // The compiler
    char cc[PATH_MAX];
    const char* want = cfg.cc_path[0] ? cfg.cc_path : "tcc";
    if (strchr(want, '/') != NULL) {
        if (os_abspath(want, cc, sizeof(cc)) == NULL)
            strlcpy(cc, want, sizeof(cc));
    } else if (os_programpath(want, cc, sizeof(cc)) == NULL) {
        add_info(OUT_ERROR, "Can't find the compiler %s (looked in sys and bin)", want);
        add_info(OUT_INFO, "Install tcc (programs/tinyc), or name the compiler in Settings > Compiler");
        return;
    }

    // Its arguments
    static char options[CFG_TEXT_MAX], libs[CFG_TEXT_MAX];
    strlcpy(options, cfg.cc_options, sizeof(options));
    strlcpy(libs, cfg.cc_libs, sizeof(libs));
    char xc[] = "-xc";
    char wall[] = "-Wall";
    char dash_o[] = "-o";
    char* argv[ARGS_MAX + 1];
    int argc = 0;
    argv[argc++] = cc;
    if (cfg.cc_wall)
        argv[argc++] = wall;
    argc = split_args(options, argv, argc, ARGS_MAX - 6);
    argv[argc++] = dash_o;
    argv[argc++] = job.output;
    const char* dot = strrchr(file, '.');
    if (dot == NULL || strcmp(dot, ".c") != 0)
        argv[argc++] = xc;          // tcc goes by the extension otherwise
    argv[argc++] = (char*)file;
    argc = split_args(libs, argv, argc, ARGS_MAX);
    argv[argc] = NULL;

    // The command line, as it would be typed
    char shown[OUT_COLS];
    strlcpy(shown, base_name(cc), sizeof(shown));
    for (int i = 1; i < argc; i++) {
        strlcat(shown, " ", sizeof(shown));
        strlcat(shown, argv[i] == job.output ? base_name(job.output) : argv[i], sizeof(shown));
    }
    add_info(OUT_INFO, "%s", shown);

    struct stat st;
    if (stat(job.output, &st) == 0 && unlink(job.output) < 0) {
        add_info(OUT_ERROR, "Can't replace %s: %s", base_name(job.output), strerror(errno));
        return;
    }

    if (!on_terminal(cc, argv)) {
        add_info(OUT_ERROR, "Can't start %s: %s", base_name(cc), strerror(errno));
        return;
    }
    strlcpy(job.name, file, sizeof(job.name));
    job.state = JOB_COMPILING;
    job.started_ms = os_time_ms();
}

static void finish(const char* how) {
    flush_partial();
    if (job.tty >= 0)
        ipc_close(job.tty);
    job.tty = -1;
    job_state_t was = job.state;
    job.state = JOB_IDLE;
    uint64_t ms = os_time_ms() - job.started_ms;

    if (was == JOB_RUNNING) {
        add_info(OUT_INFO, "%s %s", job.name, how ? how : "ended");
        return;
    }

    // Compiled: good if the output is there and nothing said "error:"
    struct stat st;
    bool made = stat(job.output, &st) == 0;
    if (how != NULL) {
        add_info(OUT_ERROR, "Compiling %s", how);
    } else if (made && job.errors == 0) {
        add_info(OUT_GOOD, "Built %s (%u warning%s, %u.%u s)", base_name(job.output), (unsigned)job.warnings,
                 job.warnings == 1 ? "" : "s", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
        if (job.goal == BUILD_RUN)
            run_output();
    } else {
        add_info(OUT_ERROR, "Build failed: %u error%s, %u warning%s", (unsigned)job.errors,
                 job.errors == 1 ? "" : "s", (unsigned)job.warnings, job.warnings == 1 ? "" : "s");
        if (job.errors > 0)
            add_info(OUT_INFO, "F8 goes to the next error, or click it");
    }
}

void build_poll(void) {
    char buf[IPC_MSG_MAX];
    while (job.tty >= 0) {
        ssize_t n = ipc_recv(job.tty, buf, sizeof(buf), NULL);
        if (n > 0) {
            take_output(buf, (size_t)n);
        } else if (n < 0 && errno == EAGAIN) {
            return;
        } else {
            finish(NULL);           // EPIPE: it ended, and everything it wrote is read
        }
    }
}

void build_stop(void) {
    if (job.state == JOB_IDLE)
        return;
    build_poll();                   // What it wrote before
    if (job.state != JOB_IDLE)
        finish("stopped (its terminal is closed)");
}
