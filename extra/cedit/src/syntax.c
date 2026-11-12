/*
 * C syntax highlighting: a small lexer that gives every byte of the text a
 * class (keyword, type, string ...). It is run over the text from the start
 * each time the window is drawn, one byte at a time, so it keeps its state
 * in hl_state_t and only ever looks ahead, never back.
 *
 * What it knows:
 *   - comments, block and line (a line comment ending in '\' goes on)
 *   - "strings" and 'characters', with backslash escapes
 *   - numbers: 12, 0x1F, 1.5e-3f, 10UL
 *   - keywords and the common type names (C99 / C11, plus the <stdint.h>
 *     and <stddef.h> types), true / false / NULL as constants
 *   - preprocessor lines (# first on the line) up to their end, '\'
 *     continuing them; <file> after #include is a string
 *
 * It is not a C parser: a typedef name of the program's own is plain text.
 */

#include <stddef.h>
#include <string.h>
#include "cedit.h"

enum { M_NORMAL, M_LINE_COMMENT, M_BLOCK_COMMENT, M_STRING, M_CHAR, M_ANGLE };

static const char* const keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do", "else", "enum",
    "extern", "for", "goto", "if", "inline", "register", "restrict", "return",
    "sizeof", "static", "struct", "switch", "typedef", "union", "volatile", "while",
    "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn", "_Static_assert",
    "_Thread_local", "asm", "__asm__", "__attribute__", "typeof", "__typeof__",
    "__inline", "__inline__", "__volatile__", "__restrict",
};

static const char* const types[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
    "_Bool", "_Complex", "bool", "size_t", "ssize_t", "ptrdiff_t", "intptr_t",
    "uintptr_t", "intmax_t", "uintmax_t", "wchar_t", "off_t", "pid_t", "time_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
    "uint64_t", "FILE", "va_list",
};

static const char* const constants[] = { "true", "false", "NULL" };

static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static bool in_list(const char* const* list, size_t count, const char* word, uint32_t n) {
    for (size_t k = 0; k < count; k++)
        if (list[k][0] == word[0] && strlen(list[k]) == n && memcmp(list[k], word, n) == 0)
            return true;
    return false;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void hl_init(hl_state_t* st) {
    memset(st, 0, sizeof(*st));
    st->mode = M_NORMAL;
    st->line_start = true;
}

// The line break at buf[i] follows a backslash: the line goes on
static bool continued(const char* buf, uint32_t i) {
    if (i > 0 && buf[i - 1] == '\\') return true;
    return i > 1 && buf[i - 1] == '\r' && buf[i - 2] == '\\';
}

hl_class_t hl_step(hl_state_t* st, const char* buf, uint32_t len, uint32_t i) {
    char c = buf[i];
    char next = (i + 1 < len) ? buf[i + 1] : '\0';
    hl_class_t plain = st->pp ? HL_PREPROC : HL_TEXT;

    if (c == '\n') {
        hl_class_t cls = (st->mode == M_BLOCK_COMMENT || st->mode == M_LINE_COMMENT) ? HL_COMMENT : plain;
        bool more = continued(buf, i);
        st->run = 0;
        if (!more) {
            if (st->mode != M_BLOCK_COMMENT)
                st->mode = M_NORMAL;    // An unclosed string ends with its line
            st->pp = st->pp_word = st->pp_include = false;
        }
        st->line_start = !more || !st->pp;
        st->esc = false;
        st->star = false;
        return cls;
    }

    if (st->run > 0) {
        st->run--;
        return (hl_class_t)st->run_cls;
    }

    switch (st->mode) {
        case M_LINE_COMMENT:
            return HL_COMMENT;

        case M_BLOCK_COMMENT:
            if (c == '/' && st->star)
                st->mode = M_NORMAL;
            st->star = (c == '*');
            return HL_COMMENT;

        case M_STRING:
        case M_CHAR: {
            hl_class_t cls = (st->mode == M_CHAR) ? HL_NUMBER : HL_STRING;
            if (st->esc)
                st->esc = false;
            else if (c == '\\')
                st->esc = true;
            else if (c == (st->mode == M_STRING ? '"' : '\''))
                st->mode = M_NORMAL;
            return cls;
        }

        case M_ANGLE:
            if (c == '>')
                st->mode = M_NORMAL;
            return HL_STRING;
    }

    // M_NORMAL
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
        return plain;

    bool first = st->line_start;
    st->line_start = false;

    if (c == '/' && next == '*') {
        st->mode = M_BLOCK_COMMENT;
        st->star = false;               // The opening '*' can't close it: "/*/"
        st->run = 1;
        st->run_cls = HL_COMMENT;
        return HL_COMMENT;
    }
    if (c == '/' && next == '/') {
        st->mode = M_LINE_COMMENT;
        return HL_COMMENT;
    }
    if (c == '"') {
        st->mode = M_STRING;
        st->esc = false;
        return HL_STRING;
    }
    if (c == '\'') {
        st->mode = M_CHAR;
        st->esc = false;
        return HL_NUMBER;
    }
    if (c == '#' && first) {
        st->pp = true;
        st->pp_word = true;
        return HL_PREPROC;
    }
    if (c == '<' && st->pp_include) {
        st->mode = M_ANGLE;
        return HL_STRING;
    }

    if (is_digit(c) || (c == '.' && is_digit(next))) {
        // A pp-number: digits, letters, '_', '.', and a sign after an exponent
        uint32_t j = i + 1;
        while (j < len) {
            char d = buf[j];
            char p = buf[j - 1];
            if (is_alnum(d) || d == '.')
                j++;
            else if ((d == '+' || d == '-') && (p == 'e' || p == 'E' || p == 'p' || p == 'P') &&
                     !(buf[i] == '0' && i + 1 < len && (buf[i + 1] == 'x' || buf[i + 1] == 'X') &&
                       (p == 'e' || p == 'E')))
                j++;
            else
                break;
        }
        st->run = j - i - 1;
        st->run_cls = HL_NUMBER;
        return HL_NUMBER;
    }

    if (is_alpha(c)) {
        uint32_t j = i + 1;
        while (j < len && is_alnum(buf[j]))
            j++;
        uint32_t n = j - i;
        const char* word = buf + i;
        hl_class_t cls;

        if (st->pp_word) {
            // The directive's name: #include, #define, #if ...
            st->pp_word = false;
            st->pp_include = (n == 7 && memcmp(word, "include", 7) == 0);
            cls = HL_PREPROC;
        } else if (in_list(keywords, COUNT(keywords), word, n)) {
            cls = HL_KEYWORD;
        } else if (in_list(types, COUNT(types), word, n)) {
            cls = HL_TYPE;
        } else if (in_list(constants, COUNT(constants), word, n)) {
            cls = HL_NUMBER;
        } else {
            cls = plain;
        }
        st->run = n - 1;
        st->run_cls = (uint8_t)cls;
        return cls;
    }

    st->pp_word = false;
    return plain;
}

bool hl_wanted(const char* path) {
    if (path == NULL || path[0] == '\0')
        return true;                    // Untitled: most likely C, in this editor
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    const char* dot = strrchr(name, '.');
    if (dot == NULL)
        return false;
    dot++;
    return strcmp(dot, "c") == 0 || strcmp(dot, "h") == 0 || strcmp(dot, "C") == 0 || strcmp(dot, "H") == 0;
}
