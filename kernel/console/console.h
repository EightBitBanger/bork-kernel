#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

#define CONSOLE_THREAD_BLOCK     0x01

void console_init(char* kb_string, char* kb_prompt, uint8_t kb_string_max_length, uint8_t kb_prompt_max_length);

void console_process_command(char* keyboard_str);

// The prompt is the working directory followed by '>'. Printing it first
// checks that the working directory still exists (it can be removed, or its
// device reformatted or detached, while the console sits in it) and falls
// back to the nearest existing ancestor.
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

// console_cmd_execute with PROCESS_SECURITY_DEFAULT (what a program typed
// at the prompt gets). Matches ConsoleCommandFn for the command table.
int console_cmd_execute_default(int arg_count, char** args);

#endif

