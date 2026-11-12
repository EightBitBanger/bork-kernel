/*
 * The system clipboard (clipboard.h). One item in DWM memory; the window
 * server (server.c) copies it in from and out to programs through shared
 * memory.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <os/ipc.h>

#include "clipboard.h"
#include "internal.h"

static dwm_clip_info_t current;         // type DWM_CLIP_NONE: empty
static uint8_t*        data;            // size + 1 bytes (a 0 after the data)
static uint32_t        serial;

static void next_serial(void) {
    serial++;
    if (serial == 0)
        serial = 1;
}

static bool valid(const dwm_clip_info_t* info) {
    if (info == NULL || info->type == DWM_CLIP_NONE || info->size > DWM_CLIP_MAX)
        return false;
    if (info->type == DWM_CLIP_IMAGE) {
        if (info->width == 0 || info->height == 0 ||
            info->width > DWM_CLIP_IMAGE_MAX_DIM || info->height > DWM_CLIP_IMAGE_MAX_DIM)
            return false;
        if ((uint64_t)info->width * info->height * 4u != info->size)
            return false;
    }
    return true;
}

bool clip_set_from(const dwm_clip_info_t* info, const void* bytes, uint32_t owner) {
    if (!valid(info) || (info->size > 0 && bytes == NULL)) {
        errno = EINVAL;
        return false;
    }

    uint8_t* copy = (uint8_t*)malloc((size_t)info->size + 1);
    if (copy == NULL) {
        errno = ENOMEM;
        return false;
    }
    if (info->size > 0)
        memcpy(copy, bytes, info->size);
    copy[info->size] = 0;

    free(data);
    data = copy;
    current = *info;
    current.format[DWM_CLIP_FORMAT_MAX - 1] = '\0';
    if (current.type != DWM_CLIP_IMAGE)
        current.width = current.height = 0;
    current.owner = owner;
    next_serial();
    current.serial = serial;
    return true;
}

bool dwm_clip_set(const dwm_clip_info_t* info, const void* bytes) {
    return clip_set_from(info, bytes, 0);
}

bool dwm_clip_set_text(const char* text) {
    dwm_clip_info_t info;
    memset(&info, 0, sizeof(info));
    info.type = DWM_CLIP_TEXT;
    info.size = (text != NULL) ? (uint32_t)strnlen(text, DWM_CLIP_MAX + 1) : 0;
    return dwm_clip_set(&info, text);
}

bool dwm_clip_set_files(const char* const* paths, size_t count, bool cut) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (paths[i] == NULL || paths[i][0] != '/' || strchr(paths[i], '\n') != NULL) {
            errno = EINVAL;
            return false;
        }
        total += strlen(paths[i]) + 1;
    }
    if (count == 0 || total > DWM_CLIP_MAX) {
        errno = EINVAL;
        return false;
    }

    char* list = (char*)malloc(total);
    if (list == NULL) {
        errno = ENOMEM;
        return false;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(paths[i]);
        memcpy(list + at, paths[i], n);
        at += n;
        if (i + 1 < count)
            list[at++] = '\n';
    }

    dwm_clip_info_t info;
    memset(&info, 0, sizeof(info));
    info.type  = DWM_CLIP_FILES;
    info.flags = cut ? DWM_CLIP_CUT : 0;
    info.size  = (uint32_t)at;
    bool ok = dwm_clip_set(&info, list);
    free(list);
    return ok;
}

bool dwm_clip_info(dwm_clip_info_t* out) {
    if (current.type == DWM_CLIP_NONE) {
        memset(out, 0, sizeof(*out));
        out->serial = serial;
        return false;
    }
    *out = current;
    return true;
}

const void* dwm_clip_data(void) {
    return (current.type == DWM_CLIP_NONE) ? NULL : data;
}

void dwm_clip_clear(uint32_t which) {
    if (current.type == DWM_CLIP_NONE || (which != 0 && which != current.serial))
        return;
    free(data);
    data = NULL;
    memset(&current, 0, sizeof(current));
    next_serial();
}

uint32_t dwm_clip_serial(void) {
    return serial;
}

int clip_share(void) {
    if (current.type == DWM_CLIP_NONE || current.size == 0)
        return 0;

    // A fresh block per program: what it does with its copy stays its own
    int shm = shm_create(current.size);
    if (shm < 0)
        return -1;
    void* p = shm_map(shm);
    if (p == NULL) {
        ipc_close(shm);
        return -1;
    }
    memcpy(p, data, current.size);
    shm_unmap(shm);         // The handle keeps the memory; nothing stays mapped here
    return shm;
}
