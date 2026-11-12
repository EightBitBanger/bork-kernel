/*

Reentrant C string tokenizer (state lives in the caller's cstr_tok_t, so
independent tokenizers can run concurrently; a single cstr_tok_t is not
safe to share between threads without a lock).

*/

#ifndef KERNEL_UTIL_TOK_H
#define KERNEL_UTIL_TOK_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    char* next_token;
    const char* delim;
} cstr_tok_t;

// Initializes the tokenizer with a string (modified in place) and its delimiters
void cstr_tok_init(cstr_tok_t* tokenizer, char* str, const char* delim);

// Retrieves the next token. Returns NULL when finished.
char* cstr_tok_next(cstr_tok_t* tokenizer);

#endif
