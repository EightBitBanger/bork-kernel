/*
 * Process security clearance (os/security.h) on SYS_SEC_GET / SYS_SEC_SET.
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <os/security.h>
#include <os/os.h>
#include "internal.h"

// The name each bit is shown by, and the short name it used to have
// (sec_parse() still takes it, so older text keeps working)
static const struct {
    sec_mask_t  bit;
    const char* name;
    const char* alias;
} sec_names[] = {
    { SEC_FILE_READ,  "file_read",             NULL         },
    { SEC_FILE_WRITE, "file_write",            NULL         },
    { SEC_CONSOLE,    "console",               NULL         },
    { SEC_GFX,        "graphics",              "gfx"        },
    { SEC_GFX_SCREEN, "fullscreen",            "gfx_screen" },
    { SEC_IPC,        "process communication", "ipc"        },
    { SEC_INPUT,      "input",                 NULL         },
    { SEC_EXEC,       "executable",            "exec"       },
    { SEC_ADMIN,      "administration",        "admin"      },
    { SEC_ELEVATE,    "elevate",               NULL         },
    { SEC_NET,        "network",               NULL         },
    { SEC_NET_CONFIG, "network settings",      "net_config" },
};

#define SEC_NAME_COUNT  (sizeof(sec_names) / sizeof(sec_names[0]))

//
// This process
//

sec_mask_t sec_get(void) {
    int32_t r = syscall1(SYS_SEC_GET, 0);
    return SYS_IS_ERROR(r) ? SEC_NONE : (sec_mask_t)r;
}

bool sec_has(sec_mask_t bits) {
    return (sec_get() & bits) == bits;
}

bool sec_is_admin(void) {
    return sec_has(SEC_ADMIN);
}

//
// Reading and changing masks
//

int sec_get_process(pid_t pid, sec_mask_t* out) {
    if (out == NULL || pid < 0) {
        errno = (out == NULL) ? EINVAL : ESRCH;
        return -1;
    }
    int32_t r = syscall1(SYS_SEC_GET, (uint32_t)pid);
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        return -1;
    }
    *out = (sec_mask_t)r;
    return 0;
}

int sec_set_process(pid_t pid, sec_mask_t mask, sec_mask_t* previous) {
    if (pid < 0) {
        errno = ESRCH;
        return -1;
    }
    int32_t r = syscall2(SYS_SEC_SET, (uint32_t)pid, mask);
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        return -1;
    }
    if (previous != NULL)
        *previous = (sec_mask_t)r;
    return 0;
}

static int sec_modify(pid_t pid, sec_mask_t set, sec_mask_t clear) {
    // Checked here too, so a bad bit fails the same way whatever the order
    if (((set | clear) & ~SEC_ALL) != 0) {
        errno = EINVAL;
        return -1;
    }
    sec_mask_t cur;
    if (sec_get_process(pid, &cur) < 0)
        return -1;
    sec_mask_t next = (cur | set) & ~clear;
    if (next == cur && sec_is_admin())
        return 0;           // Nothing to change (a non-admin still gets EPERM below)
    return sec_set_process(pid, next, NULL);
}

int sec_grant(pid_t pid, sec_mask_t bits)  { return sec_modify(pid, bits, 0); }
int sec_revoke(pid_t pid, sec_mask_t bits) { return sec_modify(pid, 0, bits); }
int sec_set(sec_mask_t mask)               { return sec_set_process(0, mask, NULL); }
int sec_drop(sec_mask_t bits)              { return sec_revoke(0, bits); }

//
// Elevation
//

int sec_elevate(const char* reason) {
    return __sys_ret(syscall2(SYS_ELEVATE, (uint32_t)(uintptr_t)reason, SEC_ADMIN));
}

int sec_elevate_bits(const char* reason, sec_mask_t bits) {
    if (bits == 0) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall2(SYS_ELEVATE, (uint32_t)(uintptr_t)reason, (uint32_t)bits));
}

// What writing the object at `path` needs that this process lacks. 0 when
// nothing is missing, the path doesn't exist, or it can't be looked at.
static sec_mask_t write_missing_at(const char* path) {
    os_finfo_t info;
    if (os_finfo(path, &info) < 0)
        return 0;
    sec_mask_t mine = sec_get();
    return (sec_mask_t)((info.security | info.write_security) & ~mine);
}

sec_mask_t sec_write_missing(const char* path, bool entry) {
    char full[PATH_MAX];
    if (path == NULL || os_abspath(path, full, sizeof(full)) == NULL)
        return 0;
    sec_mask_t missing = write_missing_at(full);
    if (entry) {
        char* slash = strrchr(full, '/');
        if (slash == full)
            full[1] = '\0';            // "/name": the root holds it
        else if (slash != NULL)
            *slash = '\0';
        missing |= write_missing_at(full);
    }
    return missing;
}

int sec_elevate_write(const char* reason, const char* path, bool entry) {
    sec_mask_t missing = sec_write_missing(path, entry);
    if (missing == 0)
        return 0;
    if (sec_elevate_bits(reason, missing) < 0)
        return -1;
    return (int)missing;
}

int sec_agent_register(void) {
    return __sys_ret(syscall3(SYS_ELEVATE_AGENT, SYS_ELEVATE_OP_REGISTER, 0, 0));
}

int sec_agent_next(sec_elevate_request_t* out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall3(SYS_ELEVATE_AGENT, SYS_ELEVATE_OP_NEXT, 0, (uint32_t)(uintptr_t)out));
}

int sec_agent_waiting(uint32_t id) {
    return __sys_ret(syscall3(SYS_ELEVATE_AGENT, SYS_ELEVATE_OP_STATUS, id, 0));
}

int sec_agent_answer(uint32_t id, bool allow) {
    return __sys_ret(syscall3(SYS_ELEVATE_AGENT, SYS_ELEVATE_OP_ANSWER, id, allow ? 1 : 0));
}

//
// Names
//

const char* sec_bit_name(sec_mask_t bit) {
    for (size_t i = 0; i < SEC_NAME_COUNT; i++) {
        if (sec_names[i].bit == bit)
            return sec_names[i].name;
    }
    return NULL;
}

typedef struct {
    char*  buf;
    size_t size;
    size_t len;
} sec_text_t;

static void sec_put(sec_text_t* t, const char* s) {
    for (; *s; s++, t->len++) {
        if (t->len + 1 < t->size)
            t->buf[t->len] = *s;
    }
}

int sec_format(sec_mask_t mask, char* buf, size_t size) {
    sec_text_t t = { buf, size, 0 };

    if (mask == 0) {
        sec_put(&t, "none");
    } else {
        sec_mask_t rest = mask;
        for (size_t i = 0; i < SEC_NAME_COUNT; i++) {
            if (!(mask & sec_names[i].bit))
                continue;
            if (t.len) sec_put(&t, "|");
            sec_put(&t, sec_names[i].name);
            rest &= ~sec_names[i].bit;
        }
        if (rest != 0) {
            char hex[11] = "0x";
            int n = 2;
            for (int shift = 28; shift >= 0; shift -= 4) {
                unsigned d = (rest >> shift) & 0xF;
                if (d != 0 || n > 2 || shift == 0)
                    hex[n++] = "0123456789abcdef"[d];
            }
            hex[n] = '\0';
            if (t.len) sec_put(&t, "|");
            sec_put(&t, hex);
        }
    }

    if (size > 0)
        buf[(t.len < size) ? t.len : size - 1] = '\0';
    return (int)t.len;
}

int sec_parse(const char* text, sec_mask_t* out) {
    if (text == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    sec_mask_t mask = 0;
    const char* p = text;
    for (;;) {
        while (*p == '|' || *p == ',' || *p == ' ' || *p == '\t') p++;
        if (*p == '\0')
            break;

        // A name with a space in it ("process communication"), whole
        bool spaced = false;
        for (size_t i = 0; i < SEC_NAME_COUNT && !spaced; i++) {
            const char* name = sec_names[i].name;
            size_t len = strlen(name);
            if (strchr(name, ' ') == NULL || strncasecmp(p, name, len) != 0)
                continue;
            char end = p[len];
            if (end == '\0' || end == '|' || end == ',' || end == ' ' || end == '\t') {
                mask |= sec_names[i].bit;
                p += len;
                spaced = true;
            }
        }
        if (spaced)
            continue;

        size_t n = 0;
        while (p[n] && p[n] != '|' && p[n] != ',' && p[n] != ' ' && p[n] != '\t') n++;

        char word[16];
        if (n >= sizeof(word)) {
            errno = EINVAL;
            return -1;
        }
        memcpy(word, p, n);
        word[n] = '\0';
        p += n;

        sec_mask_t bit = 0;
        bool known = false;
        for (size_t i = 0; i < SEC_NAME_COUNT && !known; i++) {
            if (strcasecmp(word, sec_names[i].name) == 0 ||
                (sec_names[i].alias != NULL && strcasecmp(word, sec_names[i].alias) == 0)) {
                bit = sec_names[i].bit;
                known = true;
            }
        }
        if (!known) {
            if (strcasecmp(word, "none") == 0)      { bit = SEC_NONE; known = true; }
            else if (strcasecmp(word, "all") == 0)  { bit = SEC_ALL;  known = true; }
            else if (strcasecmp(word, "file") == 0) { bit = SEC_FILE; known = true; }
            else if (word[0] >= '0' && word[0] <= '9') {
                char* end;
                unsigned long v = strtoul(word, &end, 0);
                if (*end == '\0') { bit = (sec_mask_t)v; known = true; }
            }
        }
        if (!known) {
            errno = EINVAL;
            return -1;
        }
        mask |= bit;
    }

    *out = mask;
    return 0;
}



