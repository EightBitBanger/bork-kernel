/*
 * cedit: what the parts of the editor share.
 *
 *   cedit.c     the window, the text and editing (grown from notepad)
 *   syntax.c    C syntax highlighting
 *   settings.c  editor and compiler settings, kept in the registry, and
 *               the window that changes them
 *   build.c     compiling with tcc, running the result, the output pane
 */

#ifndef CEDIT_H
#define CEDIT_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <os/gfx.h>
#include <os/wm.h>

//
// Settings (settings.c)
//

#define TAB_WIDTH_MIN   1
#define TAB_WIDTH_MAX   16
#define CFG_TEXT_MAX    200         // Compiler path, options, libraries, output name

// End of line written by Save
typedef enum {
    EOL_KEEP = 0,                   // As the file had it (a new file: LF)
    EOL_LF,                         // Unix
    EOL_CRLF,                       // DOS / Windows
} eol_mode_t;

typedef struct {
    // Editor
    uint32_t tab_width;             // Cells per tab stop, and spaces per indent level
    bool     use_spaces;            // Tab and auto-indent put in spaces, not '\t'
    bool     auto_indent;           // Enter keeps the indent (one more after '{')
    bool     brace_complete;        // ( [ { " ' put in their closing partner
    bool     highlight;             // C syntax colours (for .c / .h files and new ones)
    bool     line_numbers;
    uint32_t eol;                   // eol_mode_t

    // Compiler
    char     cc_path[CFG_TEXT_MAX];     // "" = tcc, wherever it is installed
    char     cc_options[CFG_TEXT_MAX];  // Before the source: -O2 -I... -D...
    char     cc_libs[CFG_TEXT_MAX];     // After the source: -lm ...
    char     cc_output[CFG_TEXT_MAX];   // "" = the source's name without .c
    bool     cc_wall;                   // -Wall
} settings_t;

extern settings_t cfg;

void settings_load(void);           // Defaults, then whatever the registry has
bool settings_save(void);

typedef enum { SETTINGS_EDITOR, SETTINGS_COMPILER } settings_page_t;

void        settings_open(settings_page_t page);
wm_window_t settings_window(void);  // 0 when it isn't up
bool        settings_dirty(void);
void        settings_paint(void);
void        settings_event(const wm_event_t* ev);
void        settings_theme_changed(void);

//
// C syntax highlighting (syntax.c)
//

typedef enum {
    HL_TEXT = 0,
    HL_KEYWORD,
    HL_TYPE,
    HL_PREPROC,
    HL_STRING,
    HL_NUMBER,                      // Numbers, character constants, true / false / NULL
    HL_COMMENT,
    HL_COUNT
} hl_class_t;

typedef struct {
    uint8_t  mode;                  // Inside a comment, string ...
    bool     line_start;            // Only blanks so far on this line
    bool     pp;                    // On a preprocessor line
    bool     pp_word;               // The directive's name comes next
    bool     pp_include;            // #include: <...> is a file name
    bool     esc;                   // String: the last byte was a backslash
    bool     star;                  // Block comment: the last byte was '*'
    uint8_t  run_cls;               // The rest of a word or number
    uint32_t run;
} hl_state_t;

void       hl_init(hl_state_t* st);
// The class of buf[i]. Call it for every byte, in order, from the start.
hl_class_t hl_step(hl_state_t* st, const char* buf, uint32_t len, uint32_t i);
// Whether a file of this name gets colours ("" = untitled: yes)
bool       hl_wanted(const char* path);

//
// Building (build.c)
//

typedef enum { BUILD_COMPILE, BUILD_RUN } build_goal_t;

void     build_start(build_goal_t goal);    // The document is saved and has a path
bool     build_busy(void);                  // Compiling, or a program runs in the pane
void     build_stop(void);                  // Close the running program's terminal
void     build_poll(void);                  // Read what the compiler / program wrote
int      build_handle(void);                // Pipe to wait on, or -1
void     build_next_error(int dir);         // F8 / Shift+F8

// The output pane, under the text
bool     pane_visible(void);
void     pane_toggle(void);
int32_t  pane_height(int32_t client_h, int32_t row_h);
void     pane_paint(gfx_surface_t* s, const gfx_rect_t* r, int32_t cw, int32_t row_h);
void     pane_mouse_down(const gfx_rect_t* r, int32_t x, int32_t y, int32_t row_h);
void     pane_wheel(int notches);
void     pane_clear(void);

//
// From cedit.c, for the others
//

extern wm_theme_t   theme;
extern wm_window_t  main_win;

const char* doc_path(void);                     // "" = untitled
bool        doc_is_modified(void);
void        doc_goto_line(uint32_t line, uint32_t column);   // 1-based
void        editor_settings_changed(void);
void        editor_redraw(void);

void notify_error(const char* title, const char* message);
bool point_in(const gfx_rect_t* r, int32_t x, int32_t y);
void draw_button(gfx_surface_t* s, const gfx_rect_t* r, const char* label, bool focused, bool pressed);

#endif
