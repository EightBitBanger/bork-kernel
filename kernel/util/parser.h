#ifndef KERNEL_UTIL_PARSER_H
#define KERNEL_UTIL_PARSER_H

#include <stdint.h>
#include <stddef.h>

// Remove any spaces at the beginning of a string (in-place)
void parse_trim_leading_spaces(char* str);

// Get the filename part of a path
void parse_get_filename(const char* path, char* buffer, uint16_t buffer_size);

// Get the parent path leading up to the current path
void parse_get_parent_path(const char* path, char* buffer, uint16_t buffer_size);

// String functions

// Convert a string to lowercase (in place)
void str_tolower(char *str);

// Convert a string to uppercase (in place)
void str_toupper(char *str);

// Replace the first occurrence of 'to_find' in 'dest' with 'to_replace', in place.
// max_size is the total size of the dest buffer.
// Returns 0 on success, -1 if not found (or to_find is empty), -2 if it won't fit.
int str_replace(char *dest, const char *to_find, const char *to_replace, int max_size);

#endif
