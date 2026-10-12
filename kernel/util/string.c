#include <stdint.h>
#include <stddef.h>

#include <kernel/util/string.h>

/*
 * NOTE: build this file with -ffreestanding -fno-builtin and, on GCC,
 * -fno-tree-loop-distribute-patterns. Otherwise GCC at -O2+ may recognise the
 * byte loops in memset/memcpy below and replace them with calls to
 * memset/memcpy themselves, i.e. infinite recursion.
 */

static inline int is_delimiter(char c, const char *delim) {
    while (*delim) {
        if (c == *delim) {
            return 1;
        }
        delim++;
    }
    return 0;
}

// String operations

size_t strlen(const char* str) {
    const char *s = str;
    while (*s != '\0') {
        s++;
    }
    return (size_t)(s - str);
}

size_t strnlen(const char *str, size_t maxlen) {
    const char *s = str;
    while (maxlen > 0 && *s != '\0') {
        s++;
        maxlen--;
    }
    return (size_t)(s - str);
}

char* strcpy(char* dest, const char* src) {
    char *ptr = dest;
    while ((*dest++ = *src++) != '\0');
    return ptr;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    // Pad out the remainder of the buffer with null bytes
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char* strcat(char* dest, const char* src) {
    char *ptr = dest + strlen(dest);
    while ((*ptr++ = *src++) != '\0');
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    char *ptr = dest + strlen(dest);
    while (n > 0 && *src != '\0') {
        *ptr++ = *src++;
        n--;
    }
    *ptr = '\0';
    return dest;
}

size_t strlcat(char* dest, const char* src, size_t size) {
    size_t dlen = strnlen(dest, size);
    size_t slen = strlen(src);

    // dest not terminated within size: nothing can be appended
    if (dlen == size) {
        return size + slen;
    }

    size_t room = size - dlen - 1;
    size_t copy = (slen < room) ? slen : room;

    memcpy(dest + dlen, src, copy);
    dest[dlen + copy] = '\0';

    return dlen + slen;
}

int strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(const unsigned char*)str1 - *(const unsigned char*)str2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }

    if (n == 0)
        return 0;

    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char* strtok(char* str, const char* delim) {
    static char *last_token = NULL;

    if (str != NULL) {
        last_token = str;
    }

    if (last_token == NULL) {
        return NULL;
    }

    // Skip leading delimiters
    while (*last_token && is_delimiter(*last_token, delim)) {
        last_token++;
    }

    if (*last_token == '\0') {
        last_token = NULL;
        return NULL;
    }

    char* token_start = last_token;

    // Find the end of the token
    while (*last_token && !is_delimiter(*last_token, delim)) {
        last_token++;
    }

    if (*last_token != '\0') {
        *last_token = '\0';
        last_token++;
    } else {
        last_token = NULL;
    }

    return token_start;
}

char* strchr(const char* str, int character) {
    while (*str != (char)character) {
        if (!*str) {
            return NULL;
        }
        str++;
    }
    return (char*)str;
}

char* strnchr(const char* str, size_t n, int character) {
    while (n > 0) {
        if (*str == (char)character) {
            return (char*)str;
        }
        if (*str == '\0') {
            return NULL;
        }
        str++;
        n--;
    }
    return NULL;
}

char* strrchr(const char* str, int character) {
    const char *last = NULL;
    char c = (char)character;

    while (*str != '\0') {
        if (*str == c) {
            last = str;
        }
        str++;
    }

    // Standard behavior: if searching for '\0', return pointer to the terminator
    if (c == '\0') {
        return (char*)str;
    }

    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) {
        return (char*)haystack;
    }
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) {
                return (char*)haystack;
            }
        }
    }
    return NULL;
}

size_t strspn(const char* str, const char *accept) {
    const char *s = str;
    while (*s != '\0' && is_delimiter(*s, accept)) {
        s++;
    }
    return (size_t)(s - str);
}

size_t strcspn(const char* str, const char *reject) {
    const char *s = str;
    while (*s != '\0' && !is_delimiter(*s, reject)) {
        s++;
    }
    return (size_t)(s - str);
}

// Memory Operations

void* memset(void* str, int value, size_t n) {
    unsigned char* ptr = (unsigned char*)str;
    while (n--) {
        *ptr++ = (unsigned char)value;
    }
    return str;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (d < s) {
        // Copy forward
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        // Copy backward to avoid overwriting src data
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

// Translation Operations

void utos(uint32_t value, char* dest) {
    char buffer[10]; // 10 digits max for uint32_t
    int i = 0;
    int d = 0;

    do {
        buffer[i++] = (char)((value % 10) + '0');
        value /= 10;
    } while (value > 0);

    // Reverse the string into destination
    while (i > 0) {
        dest[d++] = buffer[--i];
    }

    dest[d] = '\0';
}

void itos(int32_t value, char* dest) {
    if (value < 0) {
        *dest++ = '-';
        // Negate in unsigned arithmetic so INT32_MIN is handled correctly
        utos(0u - (uint32_t)value, dest);
    } else {
        utos((uint32_t)value, dest);
    }
}

int32_t stoi(const char* str) {
    uint32_t result = 0;
    int negative = 0;

    // Skip whitespace
    while (*str == ' ' || (*str >= '\t' && *str <= '\r')) {
        str++;
    }

    // Handle sign
    if (*str == '-') {
        negative = 1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    // Accumulate unsigned (wraps instead of signed-overflow UB),
    // which also allows "-2147483648" to parse correctly.
    while (*str >= '0' && *str <= '9') {
        result = result * 10u + (uint32_t)(*str - '0');
        str++;
    }

    return negative ? (int32_t)(0u - result) : (int32_t)result;
}

uint32_t stou(const char* str) {
    uint32_t result = 0;

    // Skip whitespace characters
    while (*str == ' ' || (*str >= '\t' && *str <= '\r')) {
        str++;
    }

    // Optional leading plus sign is valid for unsigned parsing
    if (*str == '+') {
        str++;
    }

    // Convert digits
    while (*str >= '0' && *str <= '9') {
        result = result * 10u + (uint32_t)(*str - '0');
        str++;
    }

    return result;
}

void itos_commas(uint32_t val, char* dest) {
    char raw[11];
    utos(val, raw);  // was itos(): values > INT32_MAX printed as negative

    int len = (int)strlen(raw);
    int comma_count = (len - 1) / 3;
    int new_len = len + comma_count;

    dest[new_len] = '\0';

    int src_i = len - 1;
    int dest_i = new_len - 1;
    int digit_count = 0;

    while (src_i >= 0) {
        if (digit_count > 0 && digit_count % 3 == 0) {
            dest[dest_i--] = ',';
        }
        dest[dest_i--] = raw[src_i--];
        digit_count++;
    }
}

uint32_t stoi_commas(const char* str) {
    if (!str) return 0;
    uint32_t val = 0;
    while (*str) {
        if (*str >= '0' && *str <= '9') {
            val = val * 10u + (uint32_t)(*str - '0');
        }
        str++;
    }
    return val;
}

static void utox(uint32_t value, char* dest) {
    static const char hex_digits[] = "0123456789ABCDEF";
    char buffer[8]; // 8 digits max for uint32_t
    int i = 0;
    int d = 0;

    do {
        buffer[i++] = hex_digits[value & 0xF];
        value >>= 4;
    } while (value > 0);

    while (i > 0) {
        dest[d++] = buffer[--i];
    }

    dest[d] = '\0';
}

void u8tox(uint8_t value, char* dest) {
    utox(value, dest);
}

void u16tox(uint16_t value, char* dest) {
    utox(value, dest);
}

void u32tox(uint32_t value, char* dest) {
    utox(value, dest);
}
