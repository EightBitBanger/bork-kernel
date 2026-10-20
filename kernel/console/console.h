#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

#define CONSOLE_THREAD_BLOCK     0x01
#define CONSOLE_THREAD_NO_BREAK  0x02     // With BLOCK: Ctrl+C doesn't kill it (the dwm)

// Key codes the line editor understands (as kb_getc returns them).
// Printable characters are themselves; extended and function keys arrive as
// their scancode << 8 (see VK_* in virtual_key.h).
#define CONSOLE_KEY_BACKSPACE    0x01
#define CONSOLE_KEY_ENTER        0x02
#define CONSOLE_KEY_EXTENDED(scancode)  ((uint16_t)((scancode) << 8))

void console_init(char* kb_string, char* kb_prompt, uint8_t kb_string_max_length, uint8_t kb_prompt_max_length);

void console_process_command(char* keyboard_str);

// Feed one key to the line editor: insert at the cursor, Backspace/Delete,
// Left/Right/Home/End to move, Up/Down to go through the last 16 commands,
// F3 to repeat the last command, Enter to run it. Thread context only.
void console_input_key(uint16_t key);

// The prompt is the working directory followed by '>'. Printing it first
// checks that the working directory still exists (it can be removed, or its
// device reformatted or detached, while the console sits in it) and falls
// back to the nearest existing ancestor. It also turns the blinking cursor
// on; it stays off while a command runs.
void console_prompt_print(void);

// Rebuild the prompt from the working directory, with the same check
void console_prompt_refresh(void);

// Replace the prompt text until the next refresh (the next print)
void console_prompt_set_string(const char* prompt);

uint8_t console_get_keyboard_string_length(void);
// Copies the input line into kb_string (kb_string_max_length + 1 bytes)
void console_get_keyboard_string(char* kb_string);

// Print one line per entry of the directory at `path` (absolute):
// permissions, name, and <DIR> or the size. False if `path` is not a
// directory.
bool console_list_directory(const char* path);

// Execute a program and block the current thread until completion.
// security is the process's SYS_SEC_* clearance mask (ProcessSecurity),
// e.g. SYS_SEC_INPUT | SYS_SEC_GFX_SCREEN. It is the whole mask, not added
// to the default: pass PROCESS_SECURITY_DEFAULT | ... to extend the default.
int console_cmd_execute(int arg_count, char** args, uint32_t security, uint8_t flags);

// console_cmd_execute for a program typed at the prompt: every SYS_SEC_* bit
// while CONFIG_CONSOLE_FULL_CLEARANCE (kernel/config.h) is set, otherwise
// PROCESS_SECURITY_DEFAULT. Matches ConsoleCommandFn for the command table.
int console_cmd_execute_default(int arg_count, char** args);

#endif
