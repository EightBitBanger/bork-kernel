#include <stdint.h>
#include <stddef.h>

#include <kernel/util/tok.h>

static inline int is_delimiter(char c, const char *delim) {
    while (*delim) {
        if (c == *delim) {
            return 1;
        }
        delim++;
    }
    return 0;
}

void cstr_tok_init(cstr_tok_t* tokenizer, char* str, const char* delim) {
    tokenizer->next_token = str;
    tokenizer->delim = delim;
}

char* cstr_tok_next(cstr_tok_t *tokenizer) {
    char *str = tokenizer->next_token;
    const char *delim = tokenizer->delim;
    
    if (str == NULL) {
        return NULL;
    }
    
    // Skip leading delimiters
    while (*str && is_delimiter(*str, delim)) {
        str++;
    }
    
    // If we hit the end while skipping delimiters, we are done
    if (*str == '\0') {
        tokenizer->next_token = NULL;
        return NULL;
    }
    
    char *token_start = str;
    
    // Find the end of the current token
    while (*str && !is_delimiter(*str, delim)) {
        str++;
    }
    
    // If there's more of the string left, null-terminate this token and save the next start
    if (*str != '\0') {
        *str = '\0';
        tokenizer->next_token = str + 1;
    } else {
        tokenizer->next_token = NULL;
    }
    
    return token_start;
}
