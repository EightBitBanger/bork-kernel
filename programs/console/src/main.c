/*
 * prompt: a command line for starting programs on the desktop.
 *
 *     prompt [folder]
 *
 * One window with a scrolling log and an input line. Type a command or a
 * program and its arguments and press Enter. The built-in commands are the
 * kernel console's (help, ls, cd, cp, type, format, ps, ...), copied into
 * commands/ (see commands/command.h); anything else is a program, started
 * beside the prompt with exactly those arguments. A line that is neither
 * says "Bad command or filename". The prompt is the working directory and
 * '>', as in the kernel console ("/mnt/ata0p1>").
 *
 *     ls
 *     cd /mnt/ata0p1
 *     notepad readme.txt
 *     /mnt/ata0p1/bin/hello --loud "two words"
 *     ../tools/imgedit 'my picture.bmp'
 *
 * Finding the program: a name with a '/' in it is a path (relative to the
 * working directory). A bare name is looked for in the working directory
 * first, then in the home disk's program folders, <home>/sys and then
 * <home>/bin (os_programpath()). Programs start in the prompt's working
 * directory (cd changes it), which is `folder` when one is given. The window title reads "console - <program>"
 * while a program runs on its terminal, and "console" otherwise.
 *
 * Arguments are split at spaces. "Double" or 'single' quotes keep spaces in
 * one argument; outside single quotes a backslash takes the next character
 * as it is (\" \' \\ \ ).
 *
 * Console programs: the program runs on a terminal this window hosts. What
 * it prints (stdout, stderr) shows in the log, and while it runs, the input
 * line goes to it: Enter sends the line to its stdin, so getchar(), fgets()
 * and scanf() work. ioctl(TIOCGWINSZ) gives the window's size in columns
 * and rows. The prompt is back once the program ends; Ctrl+D closes its
 * terminal before that (it reads end of file, and most programs end). Not
 * Ctrl+C: the kernel console takes that to kill a desktop started from it.
 * A line ending in " &" starts the program without a terminal and keeps the
 * prompt free. Windowed programs (os/process.h: notepad, anything using the
 * window manager) always start that way.
 *
 *     console --run <program> [args...]
 *
 * is how the desktop runs a console program (spawn_program()): the window
 * opens in the working directory it was given and runs <program> on its
 * terminal at once. When that program ends (or Ctrl+D cuts it off), the
 * console quits and its window closes. If it can't be started, the window
 * stays as a prompt so the error can be read.
 *
 *     console --script <file.sh>
 *     console <file.sh>
 *
 * runs a shell script: how the explorer and the desktop open a ".sh" file
 * (the registry's hkey_root/filetype/sh "open" value, os/filetype.h). The
 * window opens in the working directory it was given (the script's
 * folder) and runs the file a line at a time, each as if it were typed at
 * the prompt: built-in commands, programs (found as above, so relative to
 * the working directory, which cd changes for the lines after it), "&" to
 * not wait. A program on the terminal is waited for before the next line;
 * a windowed program is not. Empty lines and lines starting with '#' (so
 * "#!/bin/sh" too) are skipped, and "exit" ends the script early. When
 * the script ends, the console quits and its window closes. If a line
 * can't be run at all (Bad command or filename, a program that won't
 * start, the file can't be read) the rest still runs, but the window
 * stays as a prompt at the end so the error can be read.
 *
 * The second form is the same, for typing: the path is taken from the
 * working directory the console was started in, which the script keeps.
 * ("console <folder>" still opens a prompt in that folder.)
 *
 * Labels and goto, as in a Windows batch file: a line starting with ':'
 * is a label (and is skipped when reached, so "::" works as a comment),
 * and "goto name" (or "goto :name") carries on from the line after the
 * first ":name" in the file, matched without regard to case. "goto :eof"
 * ends the script. A goto to a label that isn't there ends the script as
 * a failed line. A script runs one line per turn of the main loop, so a
 * loop of gotos keeps the window answering keys; Ctrl+D stops it.
 *
 * A script typed at the prompt ("setup.sh", "./tools/build.sh") runs the
 * same way in this window, and the prompt is back when it ends, in the
 * directory it was in before. Scripts can run scripts, SCRIPT_DEPTH deep.
 * While a script runs, Enter only goes to a program it started; Ctrl+D
 * with no program running stops the script (all of them).
 *
 * Either way a script runs only when its file is marked executable, as a
 * program must be; otherwise the line fails with "not marked executable".
 *
 * Keys: Enter runs the line, Left / Right / Home / End / Backspace / Delete
 * edit it, Up / Down step through earlier lines, Escape or Ctrl+U empties
 * it, Page Up / Page Down scroll the log, Ctrl+L clears the log, Ctrl+V
 * pastes. Right click for Paste / Clear. The mouse wheel scrolls the log
 * WHEEL_LINES lines a notch (Ctrl: a page).
 *
 * Needs the dwm: started from the kernel console it says so and exits.
 * Needs SEC_GFX and SEC_IPC, and SEC_EXEC to start anything (the default
 * clearance has all three). Programs started from here get at most an
 * ordinary application's clearance, the same as from the desktop.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/os.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/ipc.h>
#include <os/filetype.h>
#include <os/process.h>
#include <os/security.h>
#include <os/wm.h>

#include "command.h"
#include "console_host.h"
#include "console_const.h"

#ifndef APP_NAME
#define APP_NAME        "prompt"
#endif

#ifndef PATH_MAX
#define PATH_MAX        256
#endif

// Window
#define WIN_W           520
#define WIN_H           300
#define WIN_MIN_W       200
#define WIN_MIN_H       100
#define PAD             6
#define SCRIPT_DEPTH    8           // Scripts running scripts
#define SCRIPT_MAX      (64 * 1024) // Bytes of one script file

#define PROMPT_END      ">"         // After the working directory: "/mnt/ata0p1>"
#define PROMPT_MAX      64          // Bytes of the prompt; a longer path keeps its end
#define LINE_MAX        512         // Bytes of one input line, terminator included
#define LOG_LINES       200         // Lines of log kept
#define LOG_TEXT        160         // Bytes of one log line, terminator included
#define HISTORY_LINES   32
#define WHEEL_LINES     3           // Log lines a wheel notch scrolls (Ctrl: a page)

// What a program started from here may hold: an ordinary application's
// clearance, never more than the prompt itself has (spawn_ex would refuse a
// bit we lack). The same mask the desktop and the explorer launch with.
#define LAUNCH_MASK     (SEC_FILE | SEC_GFX | SEC_IPC | SEC_EXEC | SEC_ELEVATE)    // The "user" policy

enum {
    CMD_PASTE = 1,
    CMD_CLEAR,
};

typedef enum {
    LOG_INPUT,      // The prompt (split bytes of it), then the line that was run
    LOG_INFO,
    LOG_ERROR,
    LOG_OUTPUT,     // A line a hosted program printed
    LOG_REPLY,      // The program's partial line, then what was typed after it
} log_kind_t;

typedef struct {
    log_kind_t kind;
    int        split;               // LOG_REPLY: bytes of text the program printed
    char       text[LOG_TEXT];
} log_line_t;

//
// State
//

static log_line_t   log_lines[LOG_LINES];   // Ring: log_first is the oldest
static int          log_first;
static int          log_count;
static int          scroll;                 // Rows scrolled back from the bottom

static char         line[LINE_MAX];         // The input line
static size_t       line_len;
static size_t       cursor;                 // Byte offset in line

static char*        history[HISTORY_LINES]; // Oldest first
static int          history_count;
static int          history_pos;            // == history_count: the line being typed
static char         history_draft[LINE_MAX];

static char         cwd[PATH_MAX];
static wm_window_t  main_win;
static wm_theme_t   theme;
static bool         running = true;
static bool         dirty;
static bool         focused = true;

// The program running on this window's terminal
static int          tty = -1;               // Our end of its pipe; -1: none
static char         tty_name[64];
static char         tty_partial[LOG_TEXT];  // Its output since the last line break
static size_t       tty_partial_len;
static size_t       tty_col;                // Where its next byte goes ('\r' moves it back to 0)
static int32_t      tty_columns, tty_rows;  // The size last told to it
static bool         exit_with_tty;          // --run: quit once the hosted program is gone
static bool         run_canceled;           // The last program run was refused at the admin prompt
static bool         line_failed;            // The last line couldn't be run (bad command, won't start)

// Scripts being run, the innermost last (console --script, or typed)
typedef struct {
    char*  text;                            // The whole file, NUL terminated
    size_t pos;                             // Where the next line starts
    int    line_no;                         // Of the line last read
    bool   restore_cwd;                     // Typed at the prompt: cd back afterwards
    char   name[64];
    char   saved_cwd[PATH_MAX];
} script_t;

static script_t     scripts[SCRIPT_DEPTH];
static int          script_depth;
static bool         exit_with_script;       // --script: quit once the script is done
static bool         script_failed;          // A line of it couldn't be run: stay open

// The prompt, as the kernel console shows it: the working directory and
// '>'. A path too long for it keeps its end, after "...".
static void prompt_text(char* out, size_t size) {
    if (size > PROMPT_MAX)
        size = PROMPT_MAX;
    size_t room = size - sizeof(PROMPT_END);
    size_t len = strlen(cwd);
    if (len <= room) {
        snprintf(out, size, "%s" PROMPT_END, cwd);
    } else {
        snprintf(out, size, "...%s" PROMPT_END, cwd + len - (room - 3));
    }
}

static void default_theme(void) {
    memset(&theme, 0, sizeof(theme));
    theme.client.background = GFX_RGB(236, 236, 236);
    theme.client.text       = GFX_RGB(20, 20, 20);
    theme.client.text_soft  = GFX_RGB(80, 80, 80);
    theme.client.text_danger = GFX_RGB(190, 30, 30);
    theme.client.accent     = GFX_RGB(40, 100, 200);
    theme.edit.background   = GFX_COLOR_WHITE;
    theme.edit.border       = GFX_RGB(140, 140, 140);
    theme.edit.text         = GFX_RGB(20, 20, 20);
    theme.edit.cursor       = GFX_RGB(20, 20, 20);
}

static void load_theme(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
}

//
// Log
//

static void log_add_split(log_kind_t kind, const char* text, int split) {
    int slot;
    if (log_count < LOG_LINES) {
        slot = (log_first + log_count) % LOG_LINES;
        log_count++;
    } else {
        slot = log_first;                   // Full: drop the oldest
        log_first = (log_first + 1) % LOG_LINES;
    }
    log_lines[slot].kind  = kind;
    log_lines[slot].split = split;
    strlcpy(log_lines[slot].text, text, sizeof(log_lines[slot].text));
    scroll = 0;                             // New output brings the view back down
    dirty = true;
}

static void log_add(log_kind_t kind, const char* text) {
    log_add_split(kind, text, 0);
}

static void log_printf(log_kind_t kind, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_printf(log_kind_t kind, const char* fmt, ...) {
    char text[LOG_TEXT];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    log_add(kind, text);
}

static const log_line_t* log_at(int i) {
    return &log_lines[(log_first + i) % LOG_LINES];
}

static void log_clear(void) {
    log_first = log_count = 0;
    scroll = 0;
    dirty = true;
}

//
// History
//

static void history_add(const char* text) {
    if (history_count > 0 && strcmp(history[history_count - 1], text) == 0)
        return;                             // Same as the last one
    char* copy = strdup(text);
    if (copy == NULL)
        return;
    if (history_count == HISTORY_LINES) {
        free(history[0]);
        memmove(history, history + 1, (HISTORY_LINES - 1) * sizeof(history[0]));
        history_count--;
    }
    history[history_count++] = copy;
}

static void line_set(const char* text) {
    strlcpy(line, text, sizeof(line));
    line_len = strlen(line);
    cursor = line_len;
    dirty = true;
}

// Up (-1) or Down (+1) through the history; the line being typed is kept
static void history_step(int dir) {
    int pos = history_pos + dir;
    if (pos < 0 || pos > history_count)
        return;
    if (history_pos == history_count)
        strlcpy(history_draft, line, sizeof(history_draft));
    history_pos = pos;
    line_set(pos == history_count ? history_draft : history[pos]);
}

//
// Running a line
//

// Split `text` into words: they go one after the
// other into `buf` (each with its terminator), `argv` points at them. Number
// of words, or -1 with a reason in `error`.
static int split_words(const char* text, char* buf, size_t buf_size, char** argv, int max_args,
                       const char** error) {
    int argc = 0;
    size_t out = 0;
    const char* p = text;

    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        if (argc == max_args) {
            *error = "too many arguments";
            return -1;
        }
        argv[argc++] = buf + out;

        char quote = 0;
        while (*p != '\0' && (quote != 0 || (*p != ' ' && *p != '\t'))) {
            char c = *p++;
            if (quote == '\'') {
                if (c == '\'') {
                    quote = 0;
                    continue;
                }
            } else if (c == '\\' && *p != '\0') {
                c = *p++;
            } else if (c == '"' || c == '\'') {
                if (quote == c) {
                    quote = 0;
                    continue;
                }
                if (quote == 0) {
                    quote = c;
                    continue;
                }
            }
            if (out + 1 >= buf_size) {
                *error = "line too long";
                return -1;
            }
            buf[out++] = c;
        }
        if (quote != 0) {
            *error = quote == '"' ? "missing closing \"" : "missing closing '";
            return -1;
        }
        buf[out++] = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static bool is_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

// Where `name` is: a path as given, else the working directory, else the
// home disk's program folders (<home>/sys, then <home>/bin). false when it
// is in none.
static bool find_program(const char* name, char* out, size_t size) {
    if (strchr(name, '/') != NULL) {
        if (os_abspath(name, out, size) == NULL)
            strlcpy(out, name, size);
        return true;                        // spawn says what's wrong with it
    }

    if (os_abspath(name, out, size) != NULL && is_file(out))
        return true;

    char installed[PATH_MAX];
    if (os_programpath(name, installed, sizeof(installed)) != NULL) {
        strlcpy(out, installed, size);
        return true;
    }
    return false;
}

static const char* spawn_error(int err) {
    switch (err) {
        case ENOENT:  return "no such file";
        case EISDIR:  return "it is a folder";
        case EACCES:  return "not marked executable, or no clearance for it";
        case ENOEXEC: return "not a program this kernel can run";
        case E2BIG:   return "too many arguments";
        case EAGAIN:  return "too many programs running";
        case ENOMEM:  return "out of memory";
        case EPERM:   return "not allowed to start programs";
        case EBUSY:   return "the administrator prompt is busy, try again in a moment";
        case ETIMEDOUT: return "nobody answered the administrator prompt";
        default:      return strerror(err);
    }
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

//
// The hosted program's terminal
//

// Columns and rows of program text that fit; 0 x 0 before the window has a surface
static void tty_size(int32_t* columns, int32_t* rows);
static void update_title(void);

static void tty_tell_size(void) {
    int32_t columns, rows;
    tty_size(&columns, &rows);
    if (tty < 0 || columns < 1 || rows < 1 || (columns == tty_columns && rows == tty_rows))
        return;
    if (tty_resize(tty, (unsigned)columns, (unsigned)rows) == 0) {
        tty_columns = columns;
        tty_rows    = rows;
    }
}

static void tty_flush_partial(void) {
    tty_partial[tty_partial_len] = '\0';
    log_add(LOG_OUTPUT, tty_partial);
    tty_partial_len = 0;
    tty_col = 0;
    tty_partial[0] = '\0';
    dirty = true;
}

// Puts one character where the terminal's column is: over what is there
// after a '\r', else at the end, wrapping at the window's width
static void tty_put(char c, size_t width) {
    if (tty_col >= width)
        tty_flush_partial();
    tty_partial[tty_col++] = c;
    if (tty_col > tty_partial_len)
        tty_partial_len = tty_col;
}

// Program output, a byte at a time: lines go to the log, wrapped at the
// window's width; what follows the last line break waits in tty_partial.
// '\r' goes back to the start of that line, so what comes next overwrites
// it in place (progress lines, "\r%79s\r" to blank one).
static void tty_output(const char* data, size_t n, size_t width) {
    if (width > sizeof(tty_partial) - 1)
        width = sizeof(tty_partial) - 1;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\n') {
            tty_flush_partial();
        } else if (c == '\r') {
            tty_col = 0;
        } else if (c == '\b') {
            if (tty_col > 0) {
                // At the end it erases, as before; inside the line it only steps back
                if (tty_col == tty_partial_len)
                    tty_partial_len--;
                tty_col--;
            }
        } else if (c == '\t') {
            do {
                tty_put(' ', width);
            } while (tty_col % 8 != 0);
        } else if (c >= ' ' && c != 0x7F) {
            tty_put((char)c, width);
        }
    }
    tty_partial[tty_partial_len] = '\0';
    dirty = true;
}

// The program's end of the terminal is gone (it ended, or we closed ours).
// `how` is said in the log, or nothing when NULL (it simply ended: the
// prompt coming back says so).
static void tty_finish(const char* how) {
    if (tty_partial_len > 0)
        tty_flush_partial();
    ipc_close(tty);
    tty = -1;
    if (how != NULL)
        log_printf(LOG_INFO, "%s %s", tty_name, how);
    console_cwd_changed();                  // It may have removed the folder we are in
    if (exit_with_tty)
        running = false;
}

// Everything the program has written so far
static void tty_pump(void) {
    char buf[IPC_MSG_MAX];
    while (tty >= 0) {
        ssize_t n = ipc_recv(tty, buf, sizeof(buf), NULL);
        if (n > 0) {
            tty_output(buf, (size_t)n, tty_columns > 0 ? (size_t)tty_columns : SYS_TTY_DEFAULT_COLUMNS);
        } else if (n < 0 && errno == EAGAIN) {
            return;
        } else {
            tty_finish(NULL);               // EPIPE: it ended and everything was read
        }
    }
}

// Enter while a program runs: the line, with what it printed before it,
// goes to the log, and the line goes to the program
static void tty_send_line(const char* text) {
    char shown[LOG_TEXT];
    int split = (int)tty_partial_len;
    snprintf(shown, sizeof(shown), "%s%s", tty_partial, text);
    tty_partial_len = 0;
    tty_col = 0;
    tty_partial[0] = '\0';
    log_add_split(LOG_REPLY, shown, split);

    char msg[LINE_MAX + 1];
    size_t n = strlcpy(msg, text, sizeof(msg) - 1);
    if (n > sizeof(msg) - 2)
        n = sizeof(msg) - 2;
    msg[n++] = '\n';
    if (ipc_send(tty, msg, n, 0) < 0) {
        if (errno == EAGAIN)
            log_add(LOG_ERROR, "The program isn't reading its input");
        else
            tty_pump();                     // It ended: say so
    }
}

//
// Running a line
//

static void run_argv(char** argv, bool detached);
static bool script_is(const char* path);
static bool script_start(const char* path, bool restore_cwd);

static void run_line(const char* text) {
    static char words[LINE_MAX];
    char* argv[SPAWN_ARG_MAX + 1];
    const char* error = NULL;

    // "program args &": no terminal
    char body[LINE_MAX];
    strlcpy(body, text, sizeof(body));
    size_t len = strlen(body);
    while (len > 0 && (body[len - 1] == ' ' || body[len - 1] == '\t'))
        len--;
    bool detached = len > 0 && body[len - 1] == '&';
    if (detached)
        len--;
    body[len] = '\0';
    text = body;

    line_failed = false;
    int argc = split_words(text, words, sizeof(words), argv, SPAWN_ARG_MAX, &error);
    if (argc < 0) {
        log_printf(LOG_ERROR, "Can't run that: %s", error);
        line_failed = true;
        return;
    }
    if (argc == 0)
        return;

    // Built-in commands (commands/command.c), with the words after the name
    const ConsoleCommand* builtin = console_command_find(argv[0]);
    if (builtin != NULL) {
        builtin->function(argc - 1, &argv[1]);
        dirty = true;
        return;
    }
    run_argv(argv, detached);
}

static void bad_command(void) {
    char text[sizeof(msg_bad_command)];
    strlcpy(text, msg_bad_command, sizeof(text));
    text[strcspn(text, "\n")] = '\0';
    log_add(LOG_ERROR, text);
    log_add(LOG_OUTPUT, "");
    line_failed = true;
}

// Start argv[0] (found as find_program does) with its arguments: on our
// terminal, or with none when `detached` or when it is a windowed program
static void run_argv(char** argv, bool detached) {
    run_canceled = false;
    line_failed = false;

    // "" or '' as the name, or nothing by that name anywhere
    char path[PATH_MAX];
    if (argv[0][0] == '\0' || !find_program(argv[0], path, sizeof(path))) {
        bad_command();
        return;
    }

    // A shell script runs here, a line at a time (script_step)
    if (script_is(path)) {
        if (!script_start(path, true))
            line_failed = true;
        return;
    }

    // argv[0] is the program's own path, the way the desktop starts them
    argv[0] = path;

    // A windowed program has its own window: it doesn't hold the prompt
    if (program_kind(path) == PROGRAM_WINDOWED)
        detached = true;

    // On a terminal: the child gets one end, we keep the other
    int ends[2] = { -1, -1 };
    if (!detached) {
        if (ipc_pipe(ends) < 0) {
            log_printf(LOG_ERROR, "%s: can't make a terminal: %s", base_name(path), strerror(errno));
            line_failed = true;
            return;
        }
        // Its size first, so the program's first TIOCGWINSZ already has it
        tty = ends[0];
        tty_columns = tty_rows = 0;
        tty_tell_size();
        tty = -1;
    }
    // A program whose file needs admin: the administrator prompt first
    // (it names the program, and us as the one starting it)
    sec_mask_t mask = sec_get() & LAUNCH_MASK;
    if (program_needs_elevation(path, mask) == 1)
        mask |= SPAWN_ELEVATE;

    pid_t pid = detached ? spawn_ex(path, argv, NULL, mask)
                         : spawn_tty(path, argv, NULL, mask, ends[1]);
    int err = errno;
    if (!detached)
        ipc_close(ends[1]);                 // The child holds its own
    if (pid < 0) {
        if (!detached)
            ipc_close(ends[0]);
        // The user said no to the prompt: they know, so nothing is said
        if (err == ECANCELED) {
            run_canceled = true;
            return;
        }
        log_printf(LOG_ERROR, "%s: %s", base_name(path), spawn_error(err));
        line_failed = true;
        return;
    }

    if (!detached) {
        tty = ends[0];
        strlcpy(tty_name, base_name(path), sizeof(tty_name));
        tty_partial_len = 0;
        tty_col = 0;
        tty_partial[0] = '\0';
        update_title();
    }
}

// A line being run, as the log shows it: after the prompt
static void log_command(const char* text) {
    char shown[LOG_TEXT];
    prompt_text(shown, sizeof(shown));
    int split = (int)strlen(shown);
    strlcat(shown, text, sizeof(shown));
    log_add_split(LOG_INPUT, shown, split);
}

//
// Scripts
//

// A shell script: named ".sh", or a file starting "#!" that isn't a program
static bool script_is(const char* path) {
    char ext[FT_KEY_MAX];
    if (filetype_extension(path, ext, sizeof(ext)) > 0 && strcmp(ext, "sh") == 0)
        return true;
    uint8_t head[2];
    int saved = errno;
    int n = filetype_read_header(path, head, sizeof(head));
    errno = saved;
    return n == 2 && head[0] == '#' && head[1] == '!';
}

// Read `path` and push it: script_step() runs it from the next turn of
// the main loop. false (said in the log) when it can't be.
static bool script_start(const char* path, bool restore_cwd) {
    const char* name = base_name(path);
    if (script_depth == SCRIPT_DEPTH) {
        log_printf(LOG_ERROR, "%s: scripts nested too deep", name);
        return false;
    }
    struct stat st;
    if (stat(path, &st) < 0) {
        log_printf(LOG_ERROR, "%s: %s", name, strerror(errno));
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        log_printf(LOG_ERROR, "%s: it is a folder", name);
        return false;
    }
    // Like a program: a script runs only when it is marked executable
    if (!(st.st_mode & S_IXUSR)) {
        log_printf(LOG_ERROR, "%s: not marked executable", name);
        return false;
    }
    if (st.st_size > SCRIPT_MAX) {
        log_printf(LOG_ERROR, "%s: too big for a script (over %d KB)", name, SCRIPT_MAX / 1024);
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_printf(LOG_ERROR, "%s: %s", name, strerror(errno));
        return false;
    }
    size_t size = (size_t)st.st_size;
    char* text = (char*)malloc(size + 1);
    if (text == NULL) {
        close(fd);
        log_printf(LOG_ERROR, "%s: out of memory", name);
        return false;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, text + got, size - got);
        if (n < 0) {
            int err = errno;
            close(fd);
            free(text);
            log_printf(LOG_ERROR, "%s: %s", name, strerror(err));
            return false;
        }
        if (n == 0)
            break;                          // Shorter than it said: run what is there
        got += (size_t)n;
    }
    close(fd);
    text[got] = '\0';

    script_t* sc = &scripts[script_depth++];
    memset(sc, 0, sizeof(*sc));
    sc->text = text;
    sc->restore_cwd = restore_cwd;
    strlcpy(sc->name, name, sizeof(sc->name));
    strlcpy(sc->saved_cwd, cwd, sizeof(sc->saved_cwd));
    update_title();
    dirty = true;
    return true;
}

// The innermost script is done (its end, exit, or stopped)
static void script_pop(void) {
    if (script_depth == 0)
        return;
    script_t* sc = &scripts[--script_depth];
    free(sc->text);
    sc->text = NULL;
    if (sc->restore_cwd && chdir(sc->saved_cwd) == 0)
        strlcpy(cwd, sc->saved_cwd, sizeof(cwd));
    console_cwd_changed();
    update_title();
    dirty = true;
}

// The last script is done: a --script window goes, unless a line of it
// couldn't be run (then it stays, as a prompt, so that can be read)
static void script_all_done(void) {
    if (!exit_with_script)
        return;
    exit_with_script = false;
    if (script_failed)
        log_add(LOG_INFO, "The script had errors. Close the window when done.");
    else
        running = false;
}

// Run the next line of the innermost script. Only while nothing runs on
// our terminal: a program it started is waited for.
static void script_step(void) {
    script_t* sc = &scripts[script_depth - 1];
    char text[LINE_MAX];
    for (;;) {
        const char* p = sc->text + sc->pos;
        if (*p == '\0') {
            script_pop();
            if (script_depth == 0)
                script_all_done();
            return;
        }
        size_t n = strcspn(p, "\n");
        sc->pos += n + (p[n] == '\n' ? 1 : 0);
        sc->line_no++;
        while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t'))
            n--;
        while (n > 0 && (*p == ' ' || *p == '\t')) {
            p++;
            n--;
        }
        if (n == 0 || *p == '#' || *p == ':')
            continue;                       // Blank, a comment, "#!/bin/sh", a :label (or "::" comment)
        if (n >= sizeof(text)) {
            log_printf(LOG_ERROR, "%s, line %d: too long", sc->name, sc->line_no);
            script_failed = true;
            continue;
        }
        memcpy(text, p, n);
        text[n] = '\0';
        break;
    }

    int depth = script_depth;
    int line_no = sc->line_no;
    char name[sizeof(sc->name)];
    strlcpy(name, sc->name, sizeof(name));

    log_command(text);
    run_line(text);
    console_cwd_changed();                  // cd moved it, or rm / format took it away
    if (line_failed) {
        // Where, under the error and before its blank line
        const log_line_t* last = log_count > 0 ? log_at(log_count - 1) : NULL;
        bool blank = last != NULL && last->kind == LOG_OUTPUT && last->text[0] == '\0';
        if (blank)
            log_count--;
        log_printf(LOG_ERROR, "(%s, line %d)", name, line_no);
        if (blank)
            log_add(LOG_OUTPUT, "");
        script_failed = true;
    }
    // exit, as the script's last word
    if (script_depth == 0 && depth > 0)
        script_all_done();
    dirty = true;
}

// Every script, at once (Ctrl+D); a --script window stays as a prompt
static void script_stop(void) {
    if (script_depth == 0)
        return;
    log_printf(LOG_INFO, "%s was stopped", scripts[script_depth - 1].name);
    while (script_depth > 0)
        script_pop();
    exit_with_script = false;
}

static void enter(void) {
    if (tty < 0 && script_depth > 0)
        return;                             // The script has the prompt: the line waits
    char text[LINE_MAX];
    strlcpy(text, line, sizeof(text));
    line_set("");
    history_pos = history_count;

    if (tty >= 0) {
        if (text[0] != '\0') {
            history_add(text);
            history_pos = history_count;
        }
        tty_send_line(text);
        return;
    }

    log_command(text);

    const char* p = text;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return;
    history_add(text);
    history_pos = history_count;
    run_line(text);
    console_cwd_changed();                  // cd moved it, or rm / format took it away
}

//
// Editing
//

static void insert_text(const char* text, size_t n) {
    if (n > sizeof(line) - 1 - line_len)
        n = sizeof(line) - 1 - line_len;
    if (n == 0)
        return;
    memmove(line + cursor + n, line + cursor, line_len - cursor + 1);
    memcpy(line + cursor, text, n);
    line_len += n;
    cursor += n;
    dirty = true;
}

static void delete_at(size_t at) {
    if (at >= line_len)
        return;
    memmove(line + at, line + at + 1, line_len - at);
    line_len--;
    dirty = true;
}

// The clipboard's text up to its first line break, tabs as spaces
static void paste(void) {
    wm_clip_info_t info;
    char* data = wm_clip_get(&info);
    if (data == NULL)
        return;
    if (info.type == WM_CLIP_TEXT || info.type == WM_CLIP_FILES) {
        size_t n = 0;
        while (data[n] != '\0' && data[n] != '\n' && data[n] != '\r')
            n++;
        for (size_t i = 0; i < n; i++)
            if (data[i] == '\t' || (unsigned char)data[i] < ' ')
                data[i] = ' ';
        insert_text(data, n);
    }
    free(data);
}

//
// Drawing
//

typedef struct {
    int32_t width, height;
    int32_t line_h;
    int32_t rows;           // Rows that fit
} metrics_t;

static metrics_t get_metrics(gfx_surface_t* s) {
    metrics_t m;
    wm_window_get_size(main_win, &m.width, &m.height);
    m.line_h = gfx_line_height(s);
    if (m.line_h < 1)
        m.line_h = 1;
    m.rows = (m.height - 2 * PAD) / m.line_h;
    if (m.rows < 1)
        m.rows = 1;
    return m;
}

static int32_t prefix_width(gfx_surface_t* s, const char* text, size_t n) {
    char tmp[LINE_MAX];
    if (n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    memcpy(tmp, text, n);
    tmp[n] = '\0';
    return gfx_text_width(s, tmp);
}

static void tty_size(int32_t* columns, int32_t* rows) {
    *columns = *rows = 0;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);
    int32_t cell = gfx_char_width(s);
    if (cell < 1)
        cell = 1;
    *columns = (m.width - 2 * PAD) / cell;
    if (*columns > LOG_TEXT - 1)
        *columns = LOG_TEXT - 1;            // What a log line holds
    *rows = m.rows;
}

static void draw_log_line(gfx_surface_t* s, const log_line_t* l, int32_t y) {
    int32_t x = PAD;
    switch (l->kind) {
        case LOG_OUTPUT:
            gfx_draw_text(s, x, y, l->text, theme.edit.text);
            break;
        case LOG_REPLY:
            gfx_draw_text_n(s, x, y, l->text, (size_t)l->split, theme.edit.text);
            gfx_draw_text(s, x + prefix_width(s, l->text, (size_t)l->split), y, l->text + l->split,
                          theme.client.accent);
            break;
        case LOG_INPUT:
            gfx_draw_text_n(s, x, y, l->text, (size_t)l->split, theme.client.accent);
            gfx_draw_text(s, x + prefix_width(s, l->text, (size_t)l->split), y, l->text + l->split,
                          theme.edit.text);
            break;
        case LOG_INFO:
            gfx_draw_text(s, x, y, l->text, theme.client.text_soft);
            break;
        case LOG_ERROR:
            gfx_draw_text(s, x, y, l->text, theme.client.text_danger);
            break;
    }
}

// The input line, scrolled sideways so the cursor stays in view. While a
// program runs it follows what the program has printed of the line so far
// (its own prompt) instead of the console's prompt.
static void draw_input(gfx_surface_t* s, const metrics_t* m, int32_t y) {
    int32_t mark_w;
    if (tty >= 0) {
        mark_w = gfx_text_width(s, tty_partial);
        gfx_draw_text(s, PAD, y, tty_partial, theme.edit.text);
    } else {
        char prompt[PROMPT_MAX];
        prompt_text(prompt, sizeof(prompt));
        mark_w = gfx_text_width(s, prompt);
        gfx_draw_text(s, PAD, y, prompt, theme.client.accent);
    }

    int32_t x0 = PAD + mark_w;
    int32_t avail = m->width - x0 - PAD - 2;
    size_t start = 0;
    while (start < cursor && prefix_width(s, line + start, cursor - start) > avail)
        start++;

    gfx_save(s);
    gfx_rect_t clip = gfx_rect(x0, y, m->width - x0 - PAD, m->line_h);
    gfx_intersect_clip(s, &clip);
    gfx_draw_text(s, x0, y, line + start, tty >= 0 ? theme.client.accent : theme.edit.text);
    if (focused) {
        // A '|' in the text's own font, centered on the caret position, so
        // it is as tall as the letters instead of the whole row
        int32_t cx = x0 + prefix_width(s, line + start, cursor - start);
        int32_t bar_x = cx - gfx_text_width(s, "|") / 2;
        gfx_draw_text(s, bar_x > x0 ? bar_x : x0, y, "|", theme.edit.cursor);
    }
    gfx_restore(s);
}

static void draw_main(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);

    gfx_clear(s, theme.edit.background);

    // Rows from the bottom: the input line, then the log going up
    int total = log_count + 1;
    int max_scroll = total > m.rows ? total - m.rows : 0;
    if (scroll > max_scroll)
        scroll = max_scroll;
    int last = total - 1 - scroll;          // Bottom row shown
    int first = last - m.rows + 1;
    if (first < 0)
        first = 0;

    int32_t y = PAD;
    for (int row = first; row <= last; row++, y += m.line_h) {
        if (row < log_count)
            draw_log_line(s, log_at(row), y);
        else
            draw_input(s, &m, y);
    }

    // When scrolled back, a mark on the right says there is more below
    if (scroll > 0)
        gfx_fill_rect(s, m.width - 4, m.height - PAD - m.line_h, 2, m.line_h, theme.client.accent);

    wm_window_commit(main_win, NULL);
}

static void update_title(void) {
    char title[sizeof(tty_name) + 16];
    if (main_win == 0)
        return;
    if (tty >= 0)
        snprintf(title, sizeof(title), "console - %s", tty_name);
    else if (script_depth > 0)
        snprintf(title, sizeof(title), "console - %s", scripts[script_depth - 1].name);
    else
        strlcpy(title, "console", sizeof(title));
    wm_window_set_title(main_win, title);
}

//
// What the built-in commands print through (commands/console_host.h)
//

void print(const char* text) {
    int32_t columns, rows;
    tty_size(&columns, &rows);
    tty_output(text, strlen(text), columns > 0 ? (size_t)columns : SYS_TTY_DEFAULT_COLUMNS);
}

void console_command_end(void) {
    if (tty_partial_len > 0)
        tty_flush_partial();
    log_add(LOG_OUTPUT, "");
}

void console_clear(void) {
    tty_partial_len = 0;
    tty_col = 0;
    tty_partial[0] = '\0';
    log_clear();
}

size_t console_columns(void) {
    int32_t columns, rows;
    tty_size(&columns, &rows);
    return columns > 0 ? (size_t)columns : SYS_TTY_DEFAULT_COLUMNS;
}

void console_flush(void) {
    if (tty_partial_len > 0)
        tty_flush_partial();
    draw_main();
}

// The working directory may be gone (removed, or its device reformatted):
// fall back to the nearest one above it that still exists
void console_cwd_changed(void) {
    char path[PATH_MAX];
    if (getcwd(path, sizeof(path)) == NULL)
        strlcpy(path, "/", sizeof(path));
    struct stat st;
    while (strcmp(path, "/") != 0 && (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))) {
        char* slash = strrchr(path, '/');
        if (slash == NULL || slash == path)
            strlcpy(path, "/", sizeof(path));
        else
            *slash = '\0';
    }
    if (chdir(path) == 0)
        strlcpy(cwd, path, sizeof(cwd));
}

void console_exit(void) {
    if (script_depth > 0)
        script_pop();                       // script_step() sees the window go if it was the last
    else
        running = false;
}

// goto: the line after ":label" in the innermost script is the next one
// script_step() runs. Labels are matched as cmd.exe does: no regard to
// case, the ':' optional in the goto, the first match from the top.
int console_goto(const char* label) {
    if (script_depth == 0) {
        print("goto only works in a script\n");
        console_command_end();
        line_failed = true;
        return -1;
    }
    script_t* sc = &scripts[script_depth - 1];
    if (*label == ':')
        label++;
    size_t label_len = strlen(label);

    // goto :eof ends the script, as in a batch file
    if (strcasecmp(label, "eof") == 0) {
        script_pop();
        return 0;
    }

    if (label_len > 0) {
        size_t pos = 0;
        int line_no = 0;
        while (sc->text[pos] != '\0') {
            const char* p = sc->text + pos;
            size_t n = strcspn(p, "\n");
            pos += n + (p[n] == '\n' ? 1 : 0);
            line_no++;
            while (n > 0 && (*p == ' ' || *p == '\t')) {
                p++;
                n--;
            }
            if (n < 1 + label_len || *p != ':')
                continue;
            char after = n > 1 + label_len ? p[1 + label_len] : '\0';
            if (strncasecmp(p + 1, label, label_len) == 0 &&
                (after == '\0' || after == ' ' || after == '\t' || after == '\r')) {
                sc->pos = pos;
                sc->line_no = line_no;
                return 0;
            }
        }
    }

    // Not there: the script ends here, the way cmd.exe stops a batch file
    char text[LOG_TEXT];
    snprintf(text, sizeof(text), "goto: no label :%s in %s\n", label, sc->name);
    print(text);
    console_command_end();
    line_failed = true;
    script_pop();
    return -1;
}

//
// Events
//

static char ctrl_letter(uint16_t key) {
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
        return (char)(key | 0x20);
    if (key >= 1 && key <= 26 && key != KEY_TAB && key != KEY_ENTER && key != KEY_BACKSPACE)
        return (char)('a' + key - 1);
    return 0;
}

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;
    gfx_surface_t* s = wm_window_surface(main_win);
    int page = s != NULL ? get_metrics(s).rows - 1 : 1;
    if (page < 1)
        page = 1;

    if (ev->mods & INPUT_MOD_CTRL) {
        switch (ctrl_letter(key)) {
            case 'd':
                if (tty >= 0) {
                    tty_pump();             // What it printed first
                    if (tty >= 0) {
                        tty_finish("was cut off (end of input)");
                        update_title();
                    }
                } else {
                    script_stop();
                }
                break;
            case 'v': paste();         break;
            case 'l': log_clear();     break;
            case 'u': line_set("");    break;
            case 'a': cursor = 0;        dirty = true; break;
            case 'e': cursor = line_len; dirty = true; break;
        }
        return;
    }

    switch (key) {
        case KEY_ENTER:
        case '\r':
        case '\n':
            enter();
            break;
        case KEY_BACKSPACE:
            if (cursor > 0) {
                cursor--;
                delete_at(cursor);
            }
            break;
        case KEY_DELETE:    delete_at(cursor);                              break;
        case KEY_LEFT:      if (cursor > 0) cursor--;        dirty = true;  break;
        case KEY_RIGHT:     if (cursor < line_len) cursor++; dirty = true;  break;
        case KEY_HOME:      cursor = 0;                      dirty = true;  break;
        case KEY_END:       cursor = line_len;               dirty = true;  break;
        case KEY_UP:        history_step(-1);                               break;
        case KEY_DOWN:      history_step(+1);                               break;
        case KEY_PAGE_UP:   scroll += page;                  dirty = true;  break;
        case KEY_PAGE_DOWN:
            scroll -= page;
            if (scroll < 0)
                scroll = 0;
            dirty = true;
            break;
        case KEY_ESCAPE:
            line_set("");
            history_pos = history_count;
            break;
        case KEY_TAB: {
            char sp = ' ';
            insert_text(&sp, 1);
            break;
        }
        default:
            if (key >= ' ' && key < 0x7F) {
                char c = (char)key;
                insert_text(&c, 1);
            }
            break;
    }
}

static void show_context_menu(int32_t x, int32_t y) {
    wm_clip_info_t info;
    bool can_paste = wm_clip_info(&info) == 0 &&
                     (info.type == WM_CLIP_TEXT || info.type == WM_CLIP_FILES);
    wm_menu_item_t items[] = {
        { "Paste", CMD_PASTE, can_paste ? 0 : WM_MENU_DISABLED },
        { NULL,    0,         WM_MENU_SEPARATOR },
        { "Clear", CMD_CLEAR, log_count > 0 ? 0 : WM_MENU_DISABLED },
    };
    wm_menu_show(main_win, x, y, items, 3);
}

// Wheel: + (toward the user) goes down, toward the newest lines
static void main_wheel(const wm_event_t* ev) {
    gfx_surface_t* s = wm_window_surface(main_win);
    if (ev->wheel == 0 || s == NULL)
        return;
    metrics_t m = get_metrics(s);
    int step = WHEEL_LINES;
    if (ev->mods & INPUT_MOD_CTRL)
        step = m.rows > 1 ? m.rows - 1 : 1;
    int total = log_count + 1;
    int max_scroll = total > m.rows ? total - m.rows : 0;
    int to = scroll - ev->wheel * step;
    if (to > max_scroll) to = max_scroll;
    if (to < 0) to = 0;
    if (to != scroll) {
        scroll = to;
        dirty = true;
    }
}

static void main_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_KEY:          main_key(ev);                    break;
        case WM_EVENT_MOUSE_WHEEL:  main_wheel(ev);                  break;
        case WM_EVENT_CONTEXT_MENU: show_context_menu(ev->x, ev->y); break;

        case WM_EVENT_MENU_COMMAND:
            if (ev->param == CMD_PASTE)
                paste();
            else if (ev->param == CMD_CLEAR)
                log_clear();
            break;

        case WM_EVENT_FOCUS_GAINED: focused = true;  dirty = true;   break;
        case WM_EVENT_FOCUS_LOST:   focused = false; dirty = true;   break;
        case WM_EVENT_RESIZED:      dirty = true; tty_tell_size();   break;

        case WM_EVENT_CLOSE:
            running = false;
            break;

        case WM_EVENT_DESTROYED:
            main_win = 0;
            running = false;
            break;
    }
}

//
// Main
//

// Started from the console with no desktop: say so at once. wm_connect(0)
// makes one attempt and doesn't wait for "sys.dwm" to show up.
#define NO_DWM_MESSAGE  "This program cannot be run in console mode\n"

static bool dwm_running(void) {
    if (wm_connect(0) == 0)
        return true;
    fputs(NO_DWM_MESSAGE, stdout);
    fflush(stdout);
    return false;
}

int main(int argc, char** argv) {
    if (!dwm_running())
        return 1;

    // console --run <program> [args...]: run it once the window is up
    bool run = argc > 2 && strcmp(argv[1], "--run") == 0;
    // console --script <file>: run it, then go
    bool script = argc > 2 && strcmp(argv[1], "--script") == 0;
    const char* script_arg = script ? argv[2] : NULL;
    // console <file.sh>: the same, in the working directory we were given.
    // console <folder>: a prompt there.
    if (!run && !script && argc > 1) {
        struct stat st;
        if (stat(argv[1], &st) == 0 && !S_ISDIR(st.st_mode)) {
            if (script_is(argv[1])) {
                script = true;
                script_arg = argv[1];
            } else {
                wm_notify_ex(WM_NOTIFY_ERROR, "Prompt", "Not a folder or a shell script", 0);
            }
        } else if (chdir(argv[1]) < 0) {
            wm_notify_ex(WM_NOTIFY_ERROR, "Prompt", "Can't open folder", 0);
        }
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        strlcpy(cwd, "/", sizeof(cwd));

    load_theme();

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = "console";
    d.x          = WM_POS_DEFAULT;
    d.y          = WM_POS_DEFAULT;
    d.width      = WIN_W;
    d.height     = WIN_H;
    d.min_width  = WIN_MIN_W;
    d.min_height = WIN_MIN_H;
    d.style      = WM_STYLE_RESIZABLE | WM_STYLE_CONTEXT_MENU;

    main_win = wm_window_create(&d);
    if (main_win == 0) {
        fprintf(stderr, APP_NAME ": can't create window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    update_title();
    draw_main();
    wm_window_focus(main_win);
    if (run) {
        run_argv(argv + 2, false);
        exit_with_tty = tty >= 0;           // Started on our terminal: we go when it does
        dirty = true;
        if (run_canceled)
            running = false;                // Nothing to host: go quietly
    } else if (script) {
        char path[PATH_MAX];
        if (os_abspath(script_arg, path, sizeof(path)) == NULL)
            strlcpy(path, script_arg, sizeof(path));
        exit_with_script = script_start(path, false);
        if (!exit_with_script)
            log_add(LOG_OUTPUT, "");
    }

    // Events are handled as they come, then the hosted program's output;
    // the window is repainted once both run dry, so a burst of keys or
    // output costs one frame
    wm_event_t ev;
    while (running) {
        int r = wm_wait_event(&ev, 0);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            if (tty >= 0) {
                tty_pump();
                if (tty < 0)
                    update_title();
            }
            // A script's next line, once its program (if any) is done.
            // One a turn, so keys and the window keep being answered.
            if (tty < 0 && script_depth > 0 && running) {
                script_step();
                if (dirty)
                    draw_main();
                continue;
            }
            if (dirty) {
                draw_main();
                continue;
            }

            // Nothing to do: sleep until the dwm or the program has more
            ipc_wait_item_t items[2] = {
                { wm_handle(), IPC_READABLE, 0 },
                { tty,         IPC_READABLE, 0 },
            };
            if (ipc_wait(items, tty >= 0 ? 2 : 1, IPC_FOREVER) < 0)
                break;
            continue;
        }

        if (ev.type == WM_EVENT_THEME_CHANGED) {
            load_theme();
            dirty = true;
        } else if (ev.window == main_win) {
            main_event(&ev);
        }
    }

    if (tty >= 0)
        ipc_close(tty);                     // It reads end of file
    while (script_depth > 0)
        free(scripts[--script_depth].text);
    for (int i = 0; i < history_count; i++)
        free(history[i]);
    wm_disconnect();
    return 0;
}
