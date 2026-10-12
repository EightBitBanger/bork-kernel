
#include <kernel/kernel.h>

#include <kernel/console/console_const.h>
#include <kernel/console/display.h>
#include <kernel/console/console.h>
#include <kernel/console/print.h>
#include <kernel/console/command.h>

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

// Read by the keyboard driver's line editor
char* keyboard_string;
uint8_t keyboard_length;
uint8_t keyboard_length_max;

char* prompt_string;
uint8_t prompt_length;
uint8_t prompt_length_max;

void console_init(char* kb_string, char* kb_prompt, uint8_t kb_string_max_length, uint8_t kb_prompt_max_length) {
    keyboard_length_max = kb_string_max_length;
    prompt_length_max = kb_prompt_max_length;
    keyboard_string = kb_string;
    prompt_string = kb_prompt;
    
    memset(keyboard_string, 0x00, keyboard_length_max);
    keyboard_length = 0;
    
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

void console_prompt_print(void) {
    console_prompt_refresh();
    print(prompt_string);
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

void console_process_command(char* keyboard_str) {
    // NULL-terminated for console_command_run, even when every slot is used
    char* args[CONSOLE_ARGS_MAX + 1] = {NULL};
    int arg_count = 0;
    
    parse_trim_leading_spaces(keyboard_str);
    if (keyboard_str[0] == ' ' || keyboard_str[0] == '\0') 
        return;
    
    cstr_tok_t tok;
    cstr_tok_init(&tok, keyboard_str, " \n\t");
    
    char* token = cstr_tok_next(&tok);
    while (token != NULL && arg_count < CONSOLE_ARGS_MAX) {
        args[arg_count++] = token;
        token = cstr_tok_next(&tok);
    }
    
    char* command = args[0];
    
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
    
    int result = -1;
    if (vfs_path_normalize(cwd, command, program, sizeof(program))) {
        args[0] = program;
        result = console_command_run(CONSOLE_CMD_EXECUTE, args);
    }
    
    // Then each PATH entry. A name with a '/' in it is a path, never
    // searched for. Relative PATH entries are skipped: what they named
    // would change with the working directory.
    if (result != 0 && strchr(command, '/') == NULL) {
        struct LocalPaths paths;
        kernel_get_local_paths(&paths);
        
        cstr_tok_t path_tok;
        cstr_tok_init(&path_tok, paths.path, ";");
        
        for (char* dir = cstr_tok_next(&path_tok); dir != NULL && result != 0; dir = cstr_tok_next(&path_tok)) {
            if (dir[0] != '/')
                continue;
            if (!vfs_path_normalize(dir, command, program, sizeof(program)))
                continue;
            args[0] = program;
            result = console_command_run(CONSOLE_CMD_EXECUTE, args);
        }
    }
    
    args[0] = command;
    if (result != 0)
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
        char size_text[12];
        
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
                utos(stats.size, size_text);
                right = size_text;
            }
        }
        
        console_print_listing_line(permissions, name, right);
    }
    return true;
}
