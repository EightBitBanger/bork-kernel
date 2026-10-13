
#include <kernel/dwm/dwm_clipboard.h>
#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/kernel.h>
#include <kernel/events.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/malloc.h>
#include <kernel/vfs/vfs.h>
#include <kernel/resources/resource_io.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/string.h>

// ==========================================
// Storage
// ==========================================
//
// The whole item is one heap block: a small header followed by the payload,
// sized exactly to the data, plus one '\0' so text is always terminated.
//
//   TEXT     data = text,                     size = text length
//   FILES    data = "path0\0path1\0...pathN\0", size = total bytes
//   IMAGE    data = width * height ARGB pixels, size = width * height * 4
//   BINARY   data = the bytes,                size = byte count
//
// There is no fixed-size buffer and no size limit beyond what malloc can
// give (unless DWM_CLIPBOARD_MAX_BYTES is set). One allocation per item
// means no fragmenting header/payload pairs and nothing to grow later.
//
// Blocks are reference counted. The clipboard itself holds one reference;
// readers take one for as long as they copy out (or hold a view). That way
// the lock is only held for a pointer swap or a counter update, never for a
// multi-megabyte memcpy, and replacing the contents while a paste is still
// reading them is safe: the old block is freed when the last reader is done.
//
// New contents are always built in a fresh block first and swapped in under
// the lock, so a failed set (bad input, out of memory) leaves the old
// contents untouched.

struct ClipBlock {
    uint32_t           refs;            // Guarded by clip_lock
    DWMClipboardFormat format;
    uint32_t           size;
    uint32_t           file_count;
    DWMClipboardFileOp file_op;
    uint16_t           image_width;
    uint16_t           image_height;
    uint32_t           sequence;        // Sequence number it was installed with
    bool               text_like;       // BINARY with no '\0' (decided once, at set time)
    uint8_t            data[] __attribute__((aligned(8)));
};

static mutex_t           clip_lock;
static struct ClipBlock* clip_current  = NULL;   // NULL = empty
static uint32_t          clip_sequence = 0;

//
// Block lifetime
//

// New block for `size` payload bytes, holding one reference. NULL when the
// size is over the cap, would overflow, or the heap is out of room.
static struct ClipBlock* clip_block_alloc(DWMClipboardFormat format, uint32_t size) {
#if DWM_CLIPBOARD_MAX_BYTES > 0
    if (size > DWM_CLIPBOARD_MAX_BYTES) return NULL;
#endif
    // header + payload + '\0' must not wrap around 32 bits
    if (size > 0xFFFFFFFFu - (uint32_t)sizeof(struct ClipBlock) - 1u) return NULL;

    struct ClipBlock* block = (struct ClipBlock*)malloc((uint32_t)sizeof(struct ClipBlock) + size + 1u);
    if (block == NULL) return NULL;

    memset(block, 0, sizeof(struct ClipBlock));
    block->refs    = 1;
    block->format  = format;
    block->size    = size;
    block->file_op = DWM_CLIPBOARD_OP_COPY;
    block->data[size] = '\0';
    return block;
}

// Drop one reference; the last one frees the block. Never call with the lock held.
static void clip_unref(struct ClipBlock* block) {
    if (block == NULL) return;

    mutex_lock(&clip_lock);
    bool last = (--block->refs == 0);
    mutex_unlock(&clip_lock);

    if (last) free(block);
}

// Take a reference to the current contents (NULL when empty)
static struct ClipBlock* clip_ref(void) {
    mutex_lock(&clip_lock);
    struct ClipBlock* block = clip_current;
    if (block != NULL) block->refs++;
    mutex_unlock(&clip_lock);
    return block;
}

// Swap in a fully built block. Its reference passes to the clipboard.
static void clip_install(struct ClipBlock* block) {
    mutex_lock(&clip_lock);
    struct ClipBlock* old = clip_current;
    clip_sequence++;
    block->sequence = clip_sequence;
    clip_current = block;
    mutex_unlock(&clip_lock);

    clip_unref(old);
}

static bool clip_block_is_text(const struct ClipBlock* block) {
    if (block == NULL) return false;
    return block->format == DWM_CLIPBOARD_TEXT  ||
           block->format == DWM_CLIPBOARD_FILES ||
           (block->format == DWM_CLIPBOARD_BINARY && block->text_like);
}

// Length of the block read as text (FILES drop their final '\0')
static uint32_t clip_block_text_length(const struct ClipBlock* block) {
    if (!clip_block_is_text(block)) return 0;
    if (block->format == DWM_CLIPBOARD_FILES) return (block->size > 0) ? block->size - 1 : 0;
    return block->size;
}

// Copy up to size - 1 bytes and terminate. FILES store their paths separated
// by '\0'; with nul_to_newline those become '\n'.
static void clip_copy_text_out(char* out, uint32_t size, const uint8_t* src,
                               uint32_t len, bool nul_to_newline) {
    if (out == NULL || size == 0) return;

    uint32_t n = (len < size - 1) ? len : size - 1;
    if (!nul_to_newline) {
        memcpy(out, src, n);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = src[i];
            out[i] = (char)((c == 0) ? '\n' : c);
        }
    }
    out[n] = '\0';
}

//
// General
//

void dwm_clipboard_init(void) {
    mutex_init(&clip_lock);
    wd_register_lock("clipboard", &clip_lock);
}

void dwm_clipboard_clear(void) {
    mutex_lock(&clip_lock);

    struct ClipBlock* old = clip_current;
    if (old == NULL) {
        mutex_unlock(&clip_lock);
        return;
    }
    clip_current = NULL;
    clip_sequence++;

    mutex_unlock(&clip_lock);

    clip_unref(old);
}

DWMClipboardFormat dwm_clipboard_get_format(void) {
    mutex_lock(&clip_lock);
    DWMClipboardFormat format = (clip_current != NULL) ? clip_current->format : DWM_CLIPBOARD_EMPTY;
    mutex_unlock(&clip_lock);
    return format;
}

bool dwm_clipboard_has_format(DWMClipboardFormat format) {
    bool result;

    mutex_lock(&clip_lock);
    if (format == DWM_CLIPBOARD_TEXT) {
        result = clip_block_is_text(clip_current);
    } else if (format == DWM_CLIPBOARD_EMPTY) {
        result = (clip_current == NULL);
    } else {
        result = (clip_current != NULL && clip_current->format == format);
    }
    mutex_unlock(&clip_lock);

    return result;
}

uint32_t dwm_clipboard_get_sequence(void) {
    mutex_lock(&clip_lock);
    uint32_t sequence = clip_sequence;
    mutex_unlock(&clip_lock);
    return sequence;
}

void dwm_clipboard_get_info(DWMClipboardInfo* out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(DWMClipboardInfo));

    mutex_lock(&clip_lock);
    struct ClipBlock* block = clip_current;
    out->format   = DWM_CLIPBOARD_EMPTY;
    out->file_op  = DWM_CLIPBOARD_OP_COPY;
    out->sequence = clip_sequence;
    if (block != NULL) {
        out->format       = block->format;
        out->size         = block->size;
        out->file_count   = block->file_count;
        out->file_op      = block->file_op;
        out->image_width  = block->image_width;
        out->image_height = block->image_height;
    }
    mutex_unlock(&clip_lock);
}

//
// Zero-copy access
//

bool dwm_clipboard_view_acquire(DWMClipboardView* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(DWMClipboardView));

    struct ClipBlock* block = clip_ref();
    if (block == NULL) return false;

    out->format       = block->format;
    out->data         = block->data;
    out->size         = block->size;
    out->file_count   = block->file_count;
    out->file_op      = block->file_op;
    out->image_width  = block->image_width;
    out->image_height = block->image_height;
    out->sequence     = block->sequence;
    out->handle       = block;
    return true;
}

void dwm_clipboard_view_release(DWMClipboardView* view) {
    if (view == NULL || view->handle == NULL) return;
    clip_unref((struct ClipBlock*)view->handle);
    memset(view, 0, sizeof(DWMClipboardView));
}

//
// Text
//

bool dwm_clipboard_set_text_n(const char* text, uint32_t length) {
    if (text == NULL) return false;

    length = (uint32_t)strnlen(text, length);
    if (length == 0) {
        dwm_clipboard_clear();
        return true;
    }

    struct ClipBlock* block = clip_block_alloc(DWM_CLIPBOARD_TEXT, length);
    if (block == NULL) return false;

    memcpy(block->data, text, length);
    clip_install(block);
    return true;
}

bool dwm_clipboard_set_text(const char* text) {
    return dwm_clipboard_set_text_n(text, 0xFFFFFFFFu);
}

uint32_t dwm_clipboard_get_text(char* out, uint32_t size) {
    if (out != NULL && size > 0) out[0] = '\0';

    struct ClipBlock* block = clip_ref();
    if (block == NULL) return 0;

    uint32_t length = clip_block_text_length(block);
    if (length > 0) {
        // Copied without the lock: the reference keeps the block alive
        clip_copy_text_out(out, size, block->data, length,
                           block->format == DWM_CLIPBOARD_FILES);
    }

    clip_unref(block);
    return length;
}

//
// File paths
//

bool dwm_clipboard_set_files(const char* const* paths, uint32_t count, DWMClipboardFileOp op) {
    if (paths == NULL || count == 0) return false;
#if DWM_CLIPBOARD_MAX_FILES > 0
    if (count > DWM_CLIPBOARD_MAX_FILES) return false;
#endif
    if (op != DWM_CLIPBOARD_OP_COPY && op != DWM_CLIPBOARD_OP_CUT) return false;

    // Validate everything before touching the clipboard
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (paths[i] == NULL) return false;

        size_t len = strnlen(paths[i], DWM_MAX_PATH_LEN);
        if (len == 0 || len >= DWM_MAX_PATH_LEN) return false;

        if (total > 0xFFFFFFFFu - (uint32_t)len - 1u) return false;
        total += (uint32_t)len + 1;
    }

    struct ClipBlock* block = clip_block_alloc(DWM_CLIPBOARD_FILES, total);
    if (block == NULL) return false;

    uint32_t offset = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t len = strnlen(paths[i], DWM_MAX_PATH_LEN);
        memcpy(&block->data[offset], paths[i], len);
        offset += (uint32_t)len;
        block->data[offset++] = '\0';
    }

    block->file_count = count;
    block->file_op    = op;
    clip_install(block);
    return true;
}

bool dwm_clipboard_set_file(const char* path, DWMClipboardFileOp op) {
    return dwm_clipboard_set_files(&path, 1, op);
}

uint32_t dwm_clipboard_get_file_count(void) {
    mutex_lock(&clip_lock);
    uint32_t count = (clip_current != NULL && clip_current->format == DWM_CLIPBOARD_FILES)
                   ? clip_current->file_count : 0;
    mutex_unlock(&clip_lock);
    return count;
}

bool dwm_clipboard_get_file(uint32_t index, char* out, uint32_t size) {
    if (out == NULL || size == 0) return false;
    out[0] = '\0';

    struct ClipBlock* block = clip_ref();
    if (block == NULL) return false;

    bool ok = false;
    if (block->format == DWM_CLIPBOARD_FILES && index < block->file_count) {
        const char* path = (const char*)block->data;
        for (uint32_t i = 0; i < index; i++)
            path += strlen(path) + 1;

        size_t len = strlen(path);
        if (len < size) {
            memcpy(out, path, len + 1);
            ok = true;
        }
    }

    clip_unref(block);
    return ok;
}

bool dwm_clipboard_files_complete_cut(uint32_t sequence) {
    mutex_lock(&clip_lock);

    struct ClipBlock* old = clip_current;
    if (old == NULL ||
        clip_sequence != sequence ||
        old->format != DWM_CLIPBOARD_FILES ||
        old->file_op != DWM_CLIPBOARD_OP_CUT) {
        mutex_unlock(&clip_lock);
        return false;
    }

    // The cut paths no longer exist at their old location
    clip_current = NULL;
    clip_sequence++;

    mutex_unlock(&clip_lock);

    clip_unref(old);
    return true;
}

//
// Images
//

bool dwm_clipboard_set_image(const uint32_t* pixels, uint16_t width, uint16_t height) {
    if (pixels == NULL || width == 0 || height == 0) return false;

    // 64-bit: 65535 * 65535 * 4 overflows 32 bits
    uint64_t bytes = (uint64_t)width * height * sizeof(uint32_t);
    if (bytes > 0xFFFFFFFFu) return false;

    struct ClipBlock* block = clip_block_alloc(DWM_CLIPBOARD_IMAGE, (uint32_t)bytes);
    if (block == NULL) return false;

    memcpy(block->data, pixels, (uint32_t)bytes);
    block->image_width  = width;
    block->image_height = height;
    clip_install(block);
    return true;
}

bool dwm_clipboard_get_image(struct Image* out) {
    if (out == NULL) return false;
    out->data   = NULL;
    out->width  = 0;
    out->height = 0;

    struct ClipBlock* block = clip_ref();
    if (block == NULL) return false;

    bool ok = false;
    if (block->format == DWM_CLIPBOARD_IMAGE) {
        uint32_t* pixels = (uint32_t*)malloc(block->size);
        if (pixels != NULL) {
            memcpy(pixels, block->data, block->size);
            out->data   = pixels;
            out->width  = block->image_width;
            out->height = block->image_height;
            ok = true;
        }
    }

    clip_unref(block);
    return ok;
}

//
// Raw bytes
//

bool dwm_clipboard_set(const void* data, uint32_t size) {
    if (data == NULL || size == 0) {
        dwm_clipboard_clear();
        return true;
    }

    struct ClipBlock* block = clip_block_alloc(DWM_CLIPBOARD_BINARY, size);
    if (block == NULL) return false;   // Keep the old contents on failure
    memcpy(block->data, data, size);

    // Decided once here instead of rescanning megabytes under the lock on
    // every has_format / get_text call
    block->text_like = true;
    for (uint32_t i = 0; i < size; i++) {
        if (block->data[i] == 0) { block->text_like = false; break; }
    }

    clip_install(block);
    return true;
}

uint32_t dwm_clipboard_get_data(void* out, uint32_t size) {
    struct ClipBlock* block = clip_ref();
    if (block == NULL) return 0;

    uint32_t total = 0;
    if (block->format == DWM_CLIPBOARD_TEXT || block->format == DWM_CLIPBOARD_BINARY) {
        total = block->size;
        if (out != NULL && size > 0)
            memcpy(out, block->data, (total < size) ? total : size);
    }

    clip_unref(block);
    return total;
}

const uint8_t* dwm_clipboard_get(uint32_t* out_size) {
    const uint8_t* data = NULL;
    uint32_t size = 0;

    mutex_lock(&clip_lock);
    struct ClipBlock* block = clip_current;
    if (block != NULL &&
        (block->format == DWM_CLIPBOARD_TEXT || block->format == DWM_CLIPBOARD_BINARY)) {
        data = block->data;
        size = block->size;
    }
    mutex_unlock(&clip_lock);

    if (out_size != NULL) *out_size = size;
    return data;
}

// ==========================================
// File commands (Cut / Copy / Paste menu items)
// ==========================================
//
// Pasting files hands the work to a copy window (dwm/windows/filecopy.c),
// which runs it on its own thread. These run on the DWM thread with
// the DWM lock held (they also take it themselves).

#define PASTE_TEXT_NAME      "clipping"   // File created when pasting text

static const char* paste_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static bool paste_join(char* out, size_t size, const char* dir, const char* name) {
    size_t dir_len  = strlen(dir);
    size_t name_len = strlen(name);
    bool   slash    = (dir_len == 0 || dir[dir_len - 1] != '/');

    if (dir_len + (slash ? 1 : 0) + name_len + 1 > size) return false;

    memcpy(out, dir, dir_len);
    if (slash) out[dir_len++] = '/';
    memcpy(&out[dir_len], name, name_len + 1);
    return true;
}

// True when `path` lies inside a mounted storage device (not the mount point
// itself and not one of the kernel's own /dev, /mnt, /proc nodes). Only
// those items can be copied: knode folders are not files on a disk.
static bool paste_path_is_on_device(const char* path) {
    char prefix[DWM_MAX_PATH_LEN];
    size_t len = strnlen(path, DWM_MAX_PATH_LEN);
    if (len == 0 || len >= DWM_MAX_PATH_LEN) return false;

    for (size_t i = 1; i < len; i++) {
        if (path[i] != '/') continue;
        memcpy(prefix, path, i);
        prefix[i] = '\0';
        if (vfs_directory_check_mounted(prefix)) return true;
    }
    return false;
}

// Text paste: write the clipboard text into a new "clipping" file. Written
// straight from the clipboard's own block (a view), so a multi-megabyte
// paste does not need a second copy of the text on the heap.
static bool paste_write_text(const char* dir, char* out_path, size_t path_size, char* out_name) {
    DWMClipboardView view;
    if (!dwm_clipboard_view_acquire(&view)) return false;

    // FILES are handled by the caller before it gets here; TEXT and
    // text-like BINARY are already the exact bytes to write
    if ((view.format != DWM_CLIPBOARD_TEXT && view.format != DWM_CLIPBOARD_BINARY) ||
        view.size == 0) {
        dwm_clipboard_view_release(&view);
        return false;
    }

    if (!dwm_path_unique(dir, PASTE_TEXT_NAME, out_path, path_size, out_name)) {
        dwm_clipboard_view_release(&view);
        return false;
    }

    uint32_t length = view.size;
    bool ok = false;

    File file = vfs_open(out_path, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file != VFS_INVALID_FILE) {
        vfs_close(file);

        // Size first: writes don't reliably extend a file (see notepad_save)
        FSFileStats stats;
        if (vfs_truncate(out_path, length) && vfs_stat(out_path, &stats) && stats.size == length) {
            file = vfs_open(out_path, VFS_OPEN_READ | VFS_OPEN_WRITE);
            if (file != VFS_INVALID_FILE) {
                vfs_seek(file, 0);
                ok = resource_write_exact(file, view.data, length);
                vfs_close(file);
            }
        }

        if (!ok) vfs_remove(out_path);
    }

    dwm_clipboard_view_release(&view);
    return ok;
}

// A cut is finished once its copy window moved everything: the clipboard
// then empties (unless something else was copied in the meantime)
static void paste_cut_done(const FileCopyResult* result, void* user) {
    uint32_t sequence = (uint32_t)(uintptr_t)user;
    if (!result->cancelled && result->copied > 0 && result->failed == 0)
        dwm_clipboard_files_complete_cut(sequence);
}

bool dwm_clipboard_put_file(const char* path, DWMClipboardFileOp op) {
    DWM_GUARD();
    const char* title = (op == DWM_CLIPBOARD_OP_CUT) ? "Cut" : "Copy";

    if (path == NULL || !vfs_exists(path)) {
        dwm_notify(title, "That item no longer exists", DWM_NOTIFY_LONG_MS);
        return false;
    }
    if (strnlen(path, DWM_MAX_PATH_LEN) >= DWM_MAX_PATH_LEN) {
        dwm_notify(title, "The path is too long for the clipboard", DWM_NOTIFY_LONG_MS);
        return false;
    }
    if (vfs_directory_check_mounted(path)) {
        dwm_notify(title, "Storage devices cannot be copied or moved", DWM_NOTIFY_LONG_MS);
        return false;
    }
    if (!paste_path_is_on_device(path)) {
        dwm_notify(title, "System folders cannot be copied or moved", DWM_NOTIFY_LONG_MS);
        return false;
    }
    if (!dwm_clipboard_set_file(path, op)) {
        dwm_notify(title, "Not enough memory for the clipboard", DWM_NOTIFY_LONG_MS);
        return false;
    }
    return true;
}

bool dwm_clipboard_paste_into(const char* dest_dir, int icon_x, int icon_y) {
    DWM_GUARD();
    // Normalized destination (no trailing slash except for "/")
    char dir[DWM_MAX_PATH_LEN];
    if (dest_dir == NULL || dest_dir[0] == '\0' ||
        strnlen(dest_dir, DWM_MAX_PATH_LEN) >= DWM_MAX_PATH_LEN) {
        dwm_notify("Paste", "Cannot paste into this location", DWM_NOTIFY_LONG_MS);
        return false;
    }
    memset(dir, '\0', sizeof(dir));
    strncpy(dir, dest_dir, sizeof(dir) - 1);
    size_t dir_len = strlen(dir);
    while (dir_len > 1 && dir[dir_len - 1] == '/') dir[--dir_len] = '\0';

    if (!vfs_directory_check(dir) || (!vfs_directory_check_mounted(dir) && !paste_path_is_on_device(dir))) {
        dwm_notify("Paste", "Cannot paste into this location", DWM_NOTIFY_LONG_MS);
        return false;
    }

    DWMClipboardInfo info;
    dwm_clipboard_get_info(&info);

    // ---- Files: one copy window for everything on the clipboard ----
    if (info.format == DWM_CLIPBOARD_FILES) {
        bool cut = (info.file_op == DWM_CLIPBOARD_OP_CUT);
        // HOLD: the job does not start until every item, the desktop origin
        // and the done callback are in place. Without it the copy thread
        // could finish item 1 before item 2 was added; the later adds were
        // then refused (and the refusal ignored), silently dropping files,
        // and a fast job could even end before the cut callback was set.
        uint32_t flags = FILECOPY_RENAME | FILECOPY_HOLD | (cut ? FILECOPY_MOVE : 0);

        char src[DWM_MAX_PATH_LEN];
        char dest[FILECOPY_PATH_MAX];
        WindowHandle window = 0;
        uint32_t skipped = 0;

        for (uint32_t i = 0; i < info.file_count; i++) {
            if (!dwm_clipboard_get_file(i, src, sizeof(src)) ||
                !paste_join(dest, sizeof(dest), dir, paste_basename(src))) {
                skipped++;
                continue;
            }

            if (window == 0) {
                window = dwm_summon_filecopy(src, dest, flags);
                if (window == 0) {
                    dwm_notify("Paste", "Could not start copying", DWM_NOTIFY_LONG_MS);
                    return false;
                }
            } else if (!dwm_filecopy_add(window, src, dest)) {
                skipped++;
            }
        }

        if (window == 0) {
            dwm_notify("Paste", "The copied paths are too long for this location", DWM_NOTIFY_LONG_MS);
            return false;
        }

        dwm_filecopy_set_desktop_origin(window, icon_x, icon_y);

        // A cut that skipped items must not empty the clipboard afterwards
        if (cut && skipped == 0)
            dwm_filecopy_on_done(window, paste_cut_done, (void*)(uintptr_t)info.sequence);

        dwm_filecopy_start(window);

        if (skipped > 0)
            dwm_notify("Paste", "Some items were skipped: their paths are too long here", DWM_NOTIFY_LONG_MS);
        return true;
    }

    // ---- Text: written straight into a new file ----
    if (dwm_clipboard_has_format(DWM_CLIPBOARD_TEXT)) {
        char dest[DWM_MAX_PATH_LEN];
        char dest_name[VFS_NAME_MAX + 1];

        if (!paste_write_text(dir, dest, sizeof(dest), dest_name)) {
            dwm_notify("Paste", "The text could not be saved to a file here", DWM_NOTIFY_LONG_MS);
            return false;
        }

        char desktop_dir[DWM_MAX_PATH_LEN];
        if (dwm_desktop_get_directory(desktop_dir, sizeof(desktop_dir)) && strcmp(dir, desktop_dir) == 0)
            dwm_desktop_add_item_icon(dest, dest_name, icon_x, icon_y);

        dwm_refresh_all_windows();
        return true;
    }

    dwm_notify("Paste", (info.format == DWM_CLIPBOARD_EMPTY)
                        ? "The clipboard is empty"
                        : "The clipboard holds nothing that can be pasted here",
               DWM_NOTIFY_LONG_MS);
    return false;
}
