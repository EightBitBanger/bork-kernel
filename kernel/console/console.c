#include <kernel/kernel.h>

#include <kernel/console/console_const.h>
#include <kernel/console/display.h>
#include <kernel/console/console.h>
#include <kernel/console/print.h>
#include <kernel/console/command.h>
#include <kernel/console/virtual_key.h>

#include <kernel/vfs/vfs.h>

#include <kernel/util/string.h>
#include <kernel/util/parser.h>
#include <kernel/util/tok.h>

// Words on one command line (the command itself included)
#define CONSOLE_ARGS_MAX        16

// "ls" pads each line to this many columns, so <DIR> and file sizes line
// up in a column near the names instead of at the far right of the screen.
// Names too long for it push the right field over by one space.
#define CONSOLE_LIST_WIDTH      48

// The input line. Global: other code reads it directly.
char* keyboard_string;
uint8_t keyboard_length;
uint8_t keyboard_length_max;

char* prompt_string;
uint8_t prompt_length;
uint8_t prompt_length_max;

//
// Line editor state
//

// Insertion point in keyboard_string, 0..keyboard_length
static uint8_t keyboard_cursor = 0;

// Screen cell of input index 0, the cell right after the prompt. The line
// can go negative once a long input line has scrolled the prompt off the top.
static int32_t input_col  = 0;
static int32_t input_line = 0;

// Command history, for Up/Down and F3: a ring of the last
// CONSOLE_HISTORY_MAX commands entered. A command the same as the one before
// it is not stored twice.
#define CONSOLE_HISTORY_MAX     16

static char    history[CONSOLE_HISTORY_MAX][256];
static uint8_t history_length[CONSOLE_HISTORY_MAX];
static uint8_t history_count = 0;       // Entries stored, 0..CONSOLE_HISTORY_MAX
static uint8_t history_next  = 0;       // Slot the next command goes into

// Entry on the line while browsing: 0 = newest, -1 = not browsing (the
// line is the one being typed)
static int16_t history_browse = -1;

// The line being typed when browsing started, given back by Down past the
// newest entry
static char    history_draft[256];
static uint8_t history_draft_length = 0;

void console_init(char* kb_string, char* kb_prompt, uint8_t kb_string_max_length, uint8_t kb_prompt_max_length) {
    keyboard_length_max = kb_string_max_length;
    prompt_length_max = kb_prompt_max_length;
    keyboard_string = kb_string;
    prompt_string = kb_prompt;
    
    memset(keyboard_string, 0x00, keyboard_length_max);
    keyboard_length = 0;
    keyboard_cursor = 0;
    history_count  = 0;
    history_next   = 0;
    history_browse = -1;
    
    // Before kernel_init() and the PCI scan: the working directory is "/"
    console_prompt_set_string(msg_dir_root);
    
    display_clear();
    display_cursor_set_position(0);
    display_cursor_set_line(0);
}

//
// Working directory and prompt
//

// The working directory, made to exist again if it no longer does: walk up
// until a directory resolves. "/" always does.
static void console_working_directory(char* cwd, size_t size) {
    kernel_get_working_directory(cwd, size);
    if (vfs_directory_check(cwd))
        return;
    
    while (strcmp(cwd, "/") != 0) {
        // The parent is shorter than cwd, so it always fits in place
        if (!vfs_path_normalize(cwd, "..", cwd, size)) {
            strcpy(cwd, "/");
            break;
        }
        if (vfs_directory_check(cwd))
            break;
    }
    kernel_set_working_directory(cwd);
}

void console_prompt_set_string(const char* prompt) {
    if (prompt == NULL || prompt_string == NULL || strlen(prompt) >= prompt_length_max) 
        return;
    strcpy(prompt_string, prompt);
    prompt_length = (uint8_t)strlen(prompt);
}

// "<path>>". A path too long for the prompt buffer keeps its end, the part
// that says where you are: ".../usr/desktop>"
static void console_prompt_from_path(const char* path) {
    if (prompt_string == NULL || prompt_length_max < 8)
        return;
    
    size_t room   = (size_t)prompt_length_max - 2;     // Leaves '>' and the terminator
    size_t length = strlen(path);
    size_t at     = 0;
    
    if (length > room) {
        memcpy(prompt_string, "...", 3);
        at = 3;
        path += length - (room - 3);
        length = room - 3;
    }
    memcpy(prompt_string + at, path, length);
    at += length;
    prompt_string[at++] = '>';
    prompt_string[at]   = '\0';
    prompt_length = (uint8_t)at;
}

void console_prompt_refresh(void) {
    char cwd[VFS_PATH_MAX];
    console_working_directory(cwd, sizeof(cwd));
    console_prompt_from_path(cwd);
}

//
// Command history
//

// The nth newest entry (0 = newest). NULL past the oldest.
static const char* history_get(uint8_t n, uint8_t* length) {
    if (n >= history_count)
        return NULL;
    uint8_t slot = (uint8_t)((history_next + CONSOLE_HISTORY_MAX - 1 - n) % CONSOLE_HISTORY_MAX);
    *length = history_length[slot];
    return history[slot];
}

static void history_add(const char* line, uint8_t length) {
    // Blank lines are not worth recalling
    uint8_t i = 0;
    while (i < length && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i == length)
        return;

    uint8_t newest_length;
    const char* newest = history_get(0, &newest_length);
    if (newest != NULL && newest_length == length && memcmp(newest, line, length) == 0)
        return;

    memcpy(history[history_next], line, length);
    history_length[history_next] = length;
    history_next = (uint8_t)((history_next + 1) % CONSOLE_HISTORY_MAX);
    if (history_count < CONSOLE_HISTORY_MAX)
        history_count++;
}

// Put `text` on the input line in place of what is there, cursor at its
// end. Clipped to what the buffer holds.
static void input_replace(const char* text, uint8_t length, uint8_t limit) {
    if (length > limit)
        length = limit;
    memcpy(keyboard_string, text, length);
    keyboard_length = length;
    keyboard_string[length] = '\0';
    keyboard_cursor = length;
}

//
// Line editor
//
// The input line is drawn straight into the display grid, one cell per
// character, starting at (input_col, input_line) and wrapping at the right
// edge. Every edit rewrites only the cells from the first changed character
// to the end of the line, then puts the display cursor on the insertion
// point. print() is not used while editing, so its deferred wrap never
// gets involved.
//

// Screen cell of input index `index`. If that cell is below the last row,
// the screen scrolls up until it is on screen. False if it has scrolled off
// the top (a line longer than the whole screen).
static bool input_cell(uint32_t index, uint16_t* col, uint16_t* line) {
    int32_t cols = (int32_t)display_get_columns();
    int32_t rows = (int32_t)display_get_rows();
    if (cols <= 0 || rows <= 0)
        return false;
    
    int32_t offset = input_col + (int32_t)index;
    int32_t y      = input_line + offset / cols;
    
    while (y >= rows) {
        display_newline();      // Scroll; the text already drawn moves with it
        input_line--;
        y--;
    }
    if (y < 0)
        return false;
    
    *col  = (uint16_t)(offset % cols);
    *line = (uint16_t)y;
    return true;
}

static void input_put(uint32_t index, char ch) {
    uint16_t col, line;
    if (!input_cell(index, &col, &line))
        return;
    display_cursor_set_position(col);
    display_cursor_set_line(line);
    display_putc(ch);
}

// Rewrite the line from `from` to its end, blanking whatever is left over
// from a longer line (old_length)
static void input_redraw(uint32_t from, uint32_t old_length) {
    uint32_t end = (keyboard_length > old_length) ? keyboard_length : old_length;
    for (uint32_t i = from; i < end; i++)
        input_put(i, (i < keyboard_length) ? keyboard_string[i] : ' ');
}

// Display cursor onto the insertion point, shown solid (restarts the blink)
static void input_place_cursor(void) {
    uint16_t col, line;
    if (input_cell(keyboard_cursor, &col, &line)) {
        display_cursor_set_position(col);
        display_cursor_set_line(line);
    }
    display_cursor_blink_reset();
}

void console_prompt_print(void) {
    console_prompt_refresh();
    
    display_begin_update();
    print(prompt_string);
    
    // Where the first typed character goes. A prompt that exactly fills a
    // line leaves print() with a wrap pending; this resolves it.
    int32_t col, line;
    print_next_position(&col, &line);
    input_col  = col;
    input_line = line;
    
    // A line kept across a prompt reprint is shown again after it
    if (keyboard_cursor > keyboard_length)
        keyboard_cursor = keyboard_length;
    input_redraw(0, keyboard_length);
    input_place_cursor();
    
    display_cursor_enable(true);
    display_end_update();
}

static void console_input_enter(void) {
    // No cursor while the command runs and prints
    display_cursor_enable(false);
    
    // Leave the screen cursor after the end of the line, not wherever the
    // insertion point was
    uint16_t col = 0, line = 0;
    bool on_screen = input_cell(keyboard_length, &col, &line);
    if (on_screen) {
        display_cursor_set_position(col);
        display_cursor_set_line(line);
    }
    
    // A line that ends exactly at the right edge already put the end on a
    // fresh line; a second line break would leave a blank one
    if (!(on_screen && keyboard_length > 0 && col == 0))
        print("\n");
    
    if (keyboard_length > 0) {
        // Saved first: parsing the command cuts the buffer into words
        history_add(keyboard_string, keyboard_length);
        
        console_process_command(keyboard_string);
        if (display_cursor_get_position() != 0)
            print("\n");
    }
    
    keyboard_length = 0;
    keyboard_cursor = 0;
    keyboard_string[0] = '\0';
    history_browse = -1;
    console_prompt_print();
}

void console_input_key(uint16_t key) {
    if (keyboard_string == NULL || keyboard_length_max == 0 || key == 0)
        return;
    
    if (key == CONSOLE_KEY_ENTER) {
        console_input_enter();
        return;
    }
    
    // Longest line the buffer holds, terminator included
    uint8_t limit = (uint8_t)(keyboard_length_max - 1);
    if (keyboard_length > limit)
        keyboard_length = limit;
    if (keyboard_cursor > keyboard_length)
        keyboard_cursor = keyboard_length;
    
    uint8_t old_length = keyboard_length;
    uint8_t from       = keyboard_cursor;   // First index whose cell changes
    bool    changed    = false;
    char*   s          = keyboard_string;
    
    // One screen update for the whole edit
    display_begin_update();
    
    switch (key) {
        case CONSOLE_KEY_BACKSPACE:
            if (keyboard_cursor == 0)
                break;
            keyboard_cursor--;
            memmove(&s[keyboard_cursor], &s[keyboard_cursor + 1], keyboard_length - keyboard_cursor - 1);
            keyboard_length--;
            s[keyboard_length] = '\0';
            from = keyboard_cursor;
            changed = true;
            break;
        
        case CONSOLE_KEY_EXTENDED(VK_DELETE):
            if (keyboard_cursor >= keyboard_length)
                break;
            memmove(&s[keyboard_cursor], &s[keyboard_cursor + 1], keyboard_length - keyboard_cursor - 1);
            keyboard_length--;
            s[keyboard_length] = '\0';
            changed = true;
            break;
        
        case CONSOLE_KEY_EXTENDED(VK_LEFT):
            if (keyboard_cursor > 0)
                keyboard_cursor--;
            break;
        
        case CONSOLE_KEY_EXTENDED(VK_RIGHT):
            if (keyboard_cursor < keyboard_length)
                keyboard_cursor++;
            break;
        
        case CONSOLE_KEY_EXTENDED(VK_HOME):
            keyboard_cursor = 0;
            break;
        
        case CONSOLE_KEY_EXTENDED(VK_END):
            keyboard_cursor = keyboard_length;
            break;
        
        case CONSOLE_KEY_EXTENDED(VK_UP): {
            // One entry older. Leaving the line being typed saves it first.
            uint8_t length;
            const char* entry = history_get((uint8_t)(history_browse + 1), &length);
            if (entry == NULL)
                break;      // Already at the oldest (or no history)
            if (history_browse < 0) {
                memcpy(history_draft, s, keyboard_length);
                history_draft_length = keyboard_length;
            }
            history_browse++;
            input_replace(entry, length, limit);
            from = 0;
            changed = true;
            break;
        }

        case CONSOLE_KEY_EXTENDED(VK_DOWN): {
            // One entry newer; past the newest, the line that was being typed
            if (history_browse < 0)
                break;
            history_browse--;
            uint8_t length;
            const char* entry = (history_browse >= 0)
                ? history_get((uint8_t)history_browse, &length)
                : (length = history_draft_length, history_draft);
            input_replace(entry, length, limit);
            from = 0;
            changed = true;
            break;
        }

        case CONSOLE_KEY_EXTENDED(VK_F3): {
            // DOS style: fill in the rest of the previous command from where
            // this line ends. On an empty line that is the whole command.
            uint8_t last_length = 0;
            const char* last = history_get(0, &last_length);
            while (last != NULL && keyboard_length < last_length && keyboard_length < limit) {
                s[keyboard_length] = last[keyboard_length];
                keyboard_length++;
            }
            s[keyboard_length] = '\0';
            from = old_length;
            changed = keyboard_length != old_length;
            keyboard_cursor = keyboard_length;
            break;
        }
        
        default: {
            // Printable characters are inserted at the cursor
            if (key > 0xFF)
                break;      // Other extended / function keys: no use for them yet
            char ch = (char)key;
            if ((uint8_t)ch < 0x20 || ch == 0x7F)
                break;
            if (keyboard_length >= limit)
                break;
            
            memmove(&s[keyboard_cursor + 1], &s[keyboard_cursor], keyboard_length - keyboard_cursor);
            s[keyboard_cursor] = ch;
            keyboard_length++;
            s[keyboard_length] = '\0';
            from = keyboard_cursor;
            keyboard_cursor++;
            changed = true;
            break;
        }
    }
    
    if (changed)
        input_redraw(from, old_length);
    input_place_cursor();
    
    display_end_update();
}

//
// Input line
//

// Copies the current input line into kb_string, which must hold at least
// kb_string_max_length + 1 bytes (the size given to console_init)
void console_get_keyboard_string(char* kb_string) {
    if (kb_string == NULL || keyboard_string == NULL)
        return;
    uint8_t length = keyboard_length;
    if (length > keyboard_length_max)
        length = keyboard_length_max;
    memcpy(kb_string, keyboard_string, length);
    kb_string[length] = '\0';
}

uint8_t console_get_keyboard_string_length(void) {
    return keyboard_length;
}

//
// Commands
//

// Results of console_split_words besides a word count
#define CONSOLE_SPLIT_TOO_MANY    (-1)
#define CONSOLE_SPLIT_OPEN_QUOTE  (-2)

static bool console_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Cut `line` into words, in place, and point args[] at them (NULL after the
// last). Words are separated by spaces or tabs, with shell-like quoting:
//
//   "two words"    one word, quotes removed; \" stands for " and \\ for a backslash
//   'two words'    one word, taken exactly as written (no escapes)
//   a\ b           a backslash before a space, tab, quote or backslash
//                  makes that character part of the word; any other
//                  backslash is kept as is (so a\b stays a\b)
//   ""             an empty word
//   ab"c d"e       quoting can cover part of a word: abc de
//
// Unquoting only ever shortens a word, so the result fits where it was.
// Returns the number of words, CONSOLE_SPLIT_TOO_MANY or
// CONSOLE_SPLIT_OPEN_QUOTE.
static int console_split_words(char* line, char** args, int max_args) {
    char* in  = line;
    int   count = 0;

    while (true) {
        while (console_is_space(*in))
            in++;
        if (*in == '\0')
            break;

        if (count == max_args)
            return CONSOLE_SPLIT_TOO_MANY;

        // Writing trails reading, so each word is rebuilt where it starts
        char* word = in;
        char* out  = in;

        while (*in != '\0' && !console_is_space(*in)) {
            char c = *in;

            if (c == '"') {
                in++;
                while (*in != '"') {
                    if (*in == '\0')
                        return CONSOLE_SPLIT_OPEN_QUOTE;
                    if (*in == '\\' && (in[1] == '"' || in[1] == '\\'))
                        in++;
                    *out++ = *in++;
                }
                in++;       // Closing quote
            } else if (c == '\'') {
                in++;
                while (*in != '\'') {
                    if (*in == '\0')
                        return CONSOLE_SPLIT_OPEN_QUOTE;
                    *out++ = *in++;
                }
                in++;
            } else if (c == '\\' && (in[1] == ' ' || in[1] == '\t' || in[1] == '"' ||
                                     in[1] == '\'' || in[1] == '\\')) {
                *out++ = in[1];
                in += 2;
            } else {
                *out++ = *in++;
            }
        }

        // `in` is on the separator or the end; `out` <= `in`
        bool at_end = (*in == '\0');
        *out = '\0';
        if (!at_end)
            in++;

        args[count++] = word;
        if (at_end)
            break;
    }

    args[count] = NULL;
    return count;
}

// Run the program at args[0] with all `arg_count` words. Called through the
// command table, but with the count given rather than recounted:
// console_command_run() stops at the first empty word, and "" is a real
// argument.
static int console_run_program(int arg_count, char** args) {
    const ConsoleCommand* exec = console_command_get(CONSOLE_CMD_EXECUTE);
    if (exec == NULL || exec->function == NULL)
        return CONSOLE_CMD_NOT_FOUND;
    return exec->function(arg_count, args);
}

void console_process_command(char* keyboard_str) {
    // NULL-terminated, even when every slot is used
    char* args[CONSOLE_ARGS_MAX + 1] = {NULL};

    int arg_count = console_split_words(keyboard_str, args, CONSOLE_ARGS_MAX);
    if (arg_count == CONSOLE_SPLIT_TOO_MANY) {
        print(msg_too_many_args);
        print("\n");
        return;
    }
    if (arg_count == CONSOLE_SPLIT_OPEN_QUOTE) {
        print(msg_open_quote);
        print("\n");
        return;
    }
    if (arg_count == 0)
        return;

    char* command = args[0];
    if (command[0] == '\0') {
        // The line was "" or ''
        print(msg_bad_command);
        print("\n");
        return;
    }
    
    // Built-in commands (console/command.c)
    const ConsoleCommand* builtin = console_command_find(command);
    if (builtin != NULL) {
        builtin->function(arg_count - 1, &args[1]);
        return;
    }
    
    // A program: relative to the working directory first
    char cwd[VFS_PATH_MAX];
    char program[VFS_PATH_MAX];
    kernel_get_working_directory(cwd, sizeof(cwd));
    
    int result = CONSOLE_CMD_NOT_FOUND;
    if (vfs_path_normalize(cwd, command, program, sizeof(program))) {
        args[0] = program;
        result = console_run_program(arg_count, args);
    }
    
    // Then each PATH entry, until one has the program. A name with a '/'
    // in it is a path, never searched for. Relative PATH entries are
    // skipped: what they named would change with the working directory.
    // A program that was found but failed to start (or exited with an
    // error) stops the search: its message is already printed.
    if (result == CONSOLE_CMD_NOT_FOUND && strchr(command, '/') == NULL) {
        struct LocalPaths paths;
        kernel_get_local_paths(&paths);
        
        cstr_tok_t path_tok;
        cstr_tok_init(&path_tok, paths.path, ";");
        
        for (char* dir = cstr_tok_next(&path_tok); dir != NULL && result == CONSOLE_CMD_NOT_FOUND; dir = cstr_tok_next(&path_tok)) {
            if (dir[0] != '/')
                continue;
            if (!vfs_path_normalize(dir, command, program, sizeof(program)))
                continue;
            args[0] = program;
            result = console_run_program(arg_count, args);
        }
    }
    
    args[0] = command;
    if (result == CONSOLE_CMD_NOT_FOUND)
        print(msg_bad_command);
    
    print("\n");
}

//
// Directory listing
//

// Print one listing line: permissions, the name, then `right` (may be "")
// right-aligned at CONSOLE_LIST_WIDTH. A name too long for the screen is
// shortened with "...".
static void console_print_listing_line(const char* permissions, const char* name, const char* right) {
    size_t cols      = display_get_columns();
    size_t width     = (CONSOLE_LIST_WIDTH < cols) ? CONSOLE_LIST_WIDTH : cols;
    size_t perm_len  = strlen(permissions);
    size_t right_len = strlen(right);
    size_t name_len  = strlen(name);
    
    // Room for the name on the screen, with one space before the right field
    size_t gap  = right_len ? 1 : 0;
    size_t room = (cols > perm_len + right_len + gap) ? cols - perm_len - right_len - gap : 0;
    
    // One screen update for the whole line
    display_begin_update();
    print(permissions);
    if (name_len <= room || room < 4) {
        print(name);
    } else {
        char shown[128];
        size_t keep = room - 3;
        if (keep > sizeof(shown) - 4)
            keep = sizeof(shown) - 4;
        memcpy(shown, name, keep);
        memcpy(shown + keep, "...", 4);
        print(shown);
        name_len = keep + 3;
    }
    
    if (right_len) {
        size_t printed_len = perm_len + name_len;
        size_t spaces = (printed_len + right_len < width) ? width - printed_len - right_len : 1;
        for (size_t i = 0; i < spaces; i++)
            print(" ");
        print(right);
    }
    print("\n");
    display_end_update();
}

// Kernel folders and storage devices are listed the same way, through the
// VFS. Each step takes the VFS lock on its own and nothing is printed while
// it is held (printing may reach the DWM). An entry added or removed while
// listing can shift the rest by one, as it always could.
bool console_list_directory(const char* path) {
    if (path == NULL || !vfs_directory_check(path))
        return false;
    
    char name[VFS_NAME_MAX + 1];
    char child[VFS_PATH_MAX];
    uint32_t count = vfs_directory_get_item_count(path);
    
    for (uint32_t i = 0; i < count; i++) {
        if (!vfs_directory_get_item(path, i, name, sizeof(name)))
            continue;
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;   // Never stored, but would fold away in the path
        
        char permissions[5] = "    ";
        const char* right = "";
        char size_text[21];
        
        FSFileStats stats;
        if (vfs_path_normalize(path, name, child, sizeof(child)) && vfs_stat(child, &stats)) {
            if (stats.permissions & VFS_PERMISSION_EXECUTE) permissions[0] = 'x';
            if (stats.permissions & VFS_PERMISSION_READ)    permissions[1] = 'r';
            if (stats.permissions & VFS_PERMISSION_WRITE)   permissions[2] = 'w';
            
            if (stats.type == VFS_TYPE_MOUNT) {
                permissions[0] = 'm';
                right = msg_dir_sym;
            } else if (stats.type == VFS_TYPE_DIRECTORY) {
                right = msg_dir_sym;
            } else {
                u64tos(stats.size, size_text);
                right = size_text;
            }
        }
        
        console_print_listing_line(permissions, name, right);
    }
    return true;
}
