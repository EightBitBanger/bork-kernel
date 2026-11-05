#ifndef CONSOLE_HOST_H
#define CONSOLE_HOST_H

#include <stddef.h>

// What the console program (src/main.c) gives the commands in place of the
// kernel's print.c and display.h

// Text to the log. '\n' ends a line, '\r' goes back to its start; a line
// left open waits for more (the way print() leaves the cursor).
void print(const char* text);

// Finish an open line, then one blank line before the prompt
void console_command_end(void);

// Empty the log (cls)
void console_clear(void);

// Characters that fit on one log line
size_t console_columns(void);

// Repaint the window now, before a long blocking call (format)
void console_flush(void);

// The working directory changed or may be gone (cd, rm, format): the
// host falls back to the nearest directory that still exists
void console_cwd_changed(void);

// exit: end the script being run, or close the window at the prompt
void console_exit(void);

// goto: carry on in the script being run from the line after ":label".
// 0 done; -1 (said in the log, and the line counts as failed) when no
// script is running or the label isn't in it, which ends the script.
int console_goto(const char* label);

#endif
