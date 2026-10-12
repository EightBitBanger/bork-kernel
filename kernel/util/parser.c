#include <kernel/util/parser.h>
#include <kernel/util/string.h>

void parse_trim_leading_spaces(char* str) {
    if (!str) 
        return;
    
    size_t i = 0;
    while (str[i] == ' ') {i++;}
    
    if (i > 0) {
        memmove(str, str + i, strlen(str + i) + 1);
    }
}

void parse_get_filename(const char* path, char* buffer, uint16_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    if (!path) {
        buffer[0] = '\0';
        return;
    }
    
    // Filename starts right after the last '/', or at the start if there is none
    const char* last_slash = strrchr(path, '/');
    const char* filename_start = (last_slash != NULL) ? (last_slash + 1) : path;
    
    // Copy the filename to the buffer
    size_t i = 0;
    while (filename_start[i] != '\0' && i < (size_t)(buffer_size - 1)) {
        buffer[i] = filename_start[i];
        i++;
    }
    
    buffer[i] = '\0';
}

void parse_get_parent_path(const char* path, char* buffer, uint16_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    if (!path) {
        buffer[0] = '\0';
        return;
    }
    
    const char* last_slash = strrchr(path, '/');
    
    // Determine the length of the parent path
    size_t copy_len;
    if (last_slash == NULL) {
        // No slash found (e.g., "file" -> parent is current directory "")
        copy_len = 0;
    } else if (last_slash == path) {
        // Path is in the root directory (e.g., "/file" -> parent is "/")
        copy_len = 1;
    } else {
        // Path has a nested parent (e.g., "/mnt/file" -> parent is "/mnt")
        copy_len = (size_t)(last_slash - path);
    }
    
    // Ensure we don't overflow the buffer (leave room for the null-terminator)
    if (copy_len >= buffer_size) {
        copy_len = buffer_size - 1;
    }
    
    memcpy(buffer, path, copy_len);
    buffer[copy_len] = '\0';
}

int str_replace(char *dest, const char *to_find, const char *to_replace, int max_size) {
    if (!dest || !to_find || !to_replace || max_size <= 0 || *to_find == '\0') {
        return -1;
    }
    
    char *match = strstr(dest, to_find);
    if (match == NULL) {
        return -1; // Substring not found
    }
    
    size_t dest_len = strlen(dest);
    size_t find_len = strlen(to_find);
    size_t replace_len = strlen(to_replace);
    
    // Final length = current - old substring + new substring (+1 for terminator)
    if (dest_len - find_len + replace_len >= (size_t)max_size) {
        return -2; // Not enough buffer space
    }
    
    // Shift the suffix (including its terminator) into place, then drop in the
    // replacement. Done in place: no VLA on the (possibly tiny) kernel stack.
    char *suffix = match + find_len;
    memmove(match + replace_len, suffix, strlen(suffix) + 1);
    memcpy(match, to_replace, replace_len);
    
    return 0;
}

void str_tolower(char *str) {
    if (!str) 
        return;
    
    for (; *str != '\0'; str++) {
        if (*str >= 'A' && *str <= 'Z') {
            *str = (char)(*str + ('a' - 'A'));
        }
    }
}

void str_toupper(char *str) {
    if (!str) 
        return;
    
    for (; *str != '\0'; str++) {
        if (*str >= 'a' && *str <= 'z') {
            *str = (char)(*str - ('a' - 'A'));
        }
    }
}
