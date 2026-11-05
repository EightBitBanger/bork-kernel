#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/chardev.h>
#include <kernel/driver/driver_internal.h>
#include <kernel/driver/kdrv.h>
#include <kernel/driver/kapi.h>
#include <kernel/driver/kdrv_loader.h>
#include <kernel/driver/drvcall.h>

#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/vfs/vfs.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/util/snprint.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// The table handed to modules (kapi.h)
//

static void kapi_log(const char* format, ...) {
    char line[168];
    memcpy(line, "drv: ", 5);

    va_list args;
    va_start(args, format);
    int length = vsnprintf(line + 5, sizeof(line) - 6, format, args);
    va_end(args);
    if (length < 0)
        return;

    uint32_t end = 5 + (((uint32_t)length < sizeof(line) - 6) ? (uint32_t)length : sizeof(line) - 7);
    if (end == 5 || line[end - 1] != '\n')
        line[end++] = '\n';
    serial_write(line, end);
}

static int32_t kapi_driver_call(const char* driver, const char* function, struct DriverCall* call,
                                int32_t* result) {
    return (int32_t)driver_call(driver, function, call, DRVCALL_TRUSTED, result);
}

static void* kapi_malloc(size_t size) { return malloc(size); }
static void  kapi_free(void* ptr)     { free(ptr); }

static const struct KernelApi kernel_api = {
    .version                = KAPI_VERSION,
    .size                   = sizeof(struct KernelApi),
    .malloc                 = kapi_malloc,
    .free                   = kapi_free,
    .log                    = kapi_log,
    .vsnprintf              = vsnprintf,
    .time_ms                = timer_get_ms,
    .sleep_ms               = thread_sleep,
    .device_add_platform    = device_add_platform,
    .device_find            = device_find,
    .device_remove          = device_remove,
    .chardev_register       = chardev_register,
    .chardev_unregister     = chardev_unregister,
    .chardev_copy_from_user = chardev_copy_from_user,
    .chardev_copy_to_user   = chardev_copy_to_user,
    .driver_call            = kapi_driver_call,
};

const struct KernelApi* kapi_get(void) {
    return &kernel_api;
}

//
// Loaded modules
//

typedef struct {
    bool                 used;
    const struct Driver* driver;    // Inside the image
    uint8_t*             base;      // vmm_alloc_pages
    uint32_t             pages;
    uint32_t             calls;     // kdrv_hold without kdrv_release yet
    char                 path[VFS_PATH_MAX];
} KdrvModule;

static KdrvModule modules[KDRV_LOADED_MAX];

// One load or unload at a time. Taken before the driver lock, never inside it.
static mutex_t kdrv_mutex;

void kdrv_loader_init(void) {
    mutex_init(&kdrv_mutex);
}

typedef const struct Driver* (*KdrvEntry)(const struct KernelApi* api);

const char* kdrv_error_string(KdrvResult result) {
    switch (result) {
        case KDRV_OK:             return "ok";
        case KDRV_ERR_NOT_FOUND:  return "not found";
        case KDRV_ERR_ACCESS:     return "permission denied";
        case KDRV_ERR_IO:         return "read error";
        case KDRV_ERR_NOT_DRIVER: return "not a driver file";
        case KDRV_ERR_BAD_FORMAT: return "damaged driver file (bad header)";
        case KDRV_ERR_CHECKSUM:   return "damaged driver file (checksum)";
        case KDRV_ERR_TOO_BIG:    return "driver file too large";
        case KDRV_ERR_MISMATCH:   return "built for another CPU or kernel version";
        case KDRV_ERR_NO_MEMORY:  return "out of memory";
        case KDRV_ERR_REFUSED:    return "the driver would not start";
        case KDRV_ERR_EXISTS:     return "a driver of that name is loaded";
        case KDRV_ERR_FULL:       return "too many drivers loaded";
        case KDRV_ERR_BUILTIN:    return "built-in drivers can't be unloaded";
        case KDRV_ERR_BUSY:       return "a program is calling into it, try again";
    }
    return "error";
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// [offset, offset + length) lies inside [0, limit), without wrapping
static bool range_ok(uint32_t offset, uint32_t length, uint32_t limit) {
    return offset <= limit && length <= limit - offset;
}

KdrvResult kdrv_check(const uint8_t* file, uint32_t size) {
    if (file == NULL || size < KDRV_MAGIC_SIZE || memcmp(file, KDRV_MAGIC, KDRV_MAGIC_SIZE) != 0)
        return KDRV_ERR_NOT_DRIVER;
    if (size > KDRV_FILE_MAX)
        return KDRV_ERR_TOO_BIG;
    if (size < sizeof(KdrvHeader))
        return KDRV_ERR_BAD_FORMAT;

    KdrvHeader h;
    memcpy(&h, file, sizeof(h));
    if (h.version != KDRV_VERSION || h.header_size != sizeof(KdrvHeader) || h.flags != 0)
        return KDRV_ERR_BAD_FORMAT;

    // The name is text, terminated inside its field
    if (h.name[0] == '\0' || strnlen(h.name, KDRV_NAME_MAX) >= KDRV_NAME_MAX)
        return KDRV_ERR_BAD_FORMAT;

    if (h.image_size == 0 || h.image_size > KDRV_IMAGE_MAX || h.bss_size > KDRV_IMAGE_MAX - h.image_size)
        return KDRV_ERR_TOO_BIG;
    if (h.image_offset < sizeof(KdrvHeader) || !range_ok(h.image_offset, h.image_size, size))
        return KDRV_ERR_BAD_FORMAT;
    if (h.entry >= h.image_size)
        return KDRV_ERR_BAD_FORMAT;
    if ((h.reloc_offset & 3) != 0 || h.reloc_count > size / 4 ||
        !range_ok(h.reloc_offset, h.reloc_count * 4, size))
        return KDRV_ERR_BAD_FORMAT;
    // The two blocks don't overlap each other or the header
    if (h.reloc_count != 0 && h.reloc_offset < sizeof(KdrvHeader))
        return KDRV_ERR_BAD_FORMAT;
    if (h.reloc_count != 0 && h.reloc_offset < h.image_offset + h.image_size &&
        h.image_offset < h.reloc_offset + h.reloc_count * 4)
        return KDRV_ERR_BAD_FORMAT;

    // Every relocation patches a whole word inside the image (not the bss:
    // that is zeroed after the copy)
    for (uint32_t i = 0; i < h.reloc_count; i++) {
        uint32_t at = get_u32(file + h.reloc_offset + i * 4);
        if (!range_ok(at, 4, h.image_size))
            return KDRV_ERR_BAD_FORMAT;
    }

    // Damage before compatibility: a bad copy should say so, not "wrong CPU"
    uint8_t zero[4] = { 0, 0, 0, 0 };
    uint32_t at = (uint32_t)offsetof(KdrvHeader, checksum);
    uint32_t crc = kdrv_crc32(0, file, at);
    crc = kdrv_crc32(crc, zero, 4);
    crc = kdrv_crc32(crc, file + at + 4, size - at - 4);
    if (crc != h.checksum)
        return KDRV_ERR_CHECKSUM;

    if (h.machine != KDRV_MACHINE_I386 || h.driver_abi != DRIVER_ABI ||
        h.kapi_version == 0 || h.kapi_version > KAPI_VERSION)
        return KDRV_ERR_MISMATCH;
    return KDRV_OK;
}

// The whole file into the kernel heap, read with the caller's clearance
static KdrvResult read_file(const char* path, uint8_t** out, uint32_t* out_size) {
    FSFileStats st;
    if (path == NULL || !vfs_stat(path, &st) || st.type != VFS_TYPE_FILE)
        return KDRV_ERR_NOT_FOUND;
    if (!vfs_check_access(path))
        return KDRV_ERR_ACCESS;

    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE)
        return KDRV_ERR_ACCESS;
    if (vfs_file_is_device(file, NULL)) {
        vfs_close(file);
        return KDRV_ERR_NOT_DRIVER;
    }

    uint64_t size = vfs_get_size(file);
    if (size < KDRV_MAGIC_SIZE) {
        vfs_close(file);
        return KDRV_ERR_NOT_DRIVER;
    }
    if (size > KDRV_FILE_MAX) {
        // Still say "not a driver" for a big file that isn't one
        char magic[KDRV_MAGIC_SIZE];
        bool is_driver = vfs_read(file, magic, sizeof(magic)) == (int32_t)sizeof(magic) &&
                         memcmp(magic, KDRV_MAGIC, KDRV_MAGIC_SIZE) == 0;
        vfs_close(file);
        return is_driver ? KDRV_ERR_TOO_BIG : KDRV_ERR_NOT_DRIVER;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)size);
    if (data == NULL) {
        vfs_close(file);
        return KDRV_ERR_NO_MEMORY;
    }

    uint32_t done = 0;
    while (done < (uint32_t)size) {
        int32_t got = vfs_read(file, data + done, (uint32_t)size - done);
        if (got <= 0)
            break;
        done += (uint32_t)got;
    }
    vfs_close(file);

    if (done != (uint32_t)size) {
        free(data);
        return KDRV_ERR_IO;
    }
    *out = data;
    *out_size = done;
    return KDRV_OK;
}

static bool in_image(const void* p, uint32_t length, const uint8_t* base, uint32_t total) {
    uint32_t address = (uint32_t)p;
    return address >= (uint32_t)base && range_ok(address - (uint32_t)base, length, total);
}

// `text` is a string that ends inside the module, at most `max` bytes with
// its terminator. Never reads past the module.
static bool in_image_text(const char* text, uint32_t max, const uint8_t* base, uint32_t total) {
    if (!in_image(text, 1, base, total))
        return false;
    uint32_t left = total - (uint32_t)((const uint8_t*)text - base);
    uint32_t limit = (left < max) ? left : max;
    for (uint32_t i = 0; i < limit; i++) {
        if (text[i] == '\0')
            return true;
    }
    return false;
}

static bool in_code(const void* fn, const KdrvHeader* h, const uint8_t* base) {
    return in_image(fn, 1, base, h->image_size);
}

// A library's exports table: every row, its strings and its function
// inside the module. What the rows say is checked by driver_shape_ok.
static bool exports_sane(const struct DriverExport* row, const KdrvHeader* h, const uint8_t* base, uint32_t total) {
    for (uint32_t n = 0; ; n++, row++) {
        if (n > DRIVER_EXPORT_MAX || !in_image(row, sizeof(*row), base, total))
            return false;
        if (row->name == NULL)
            return true;
        if (!in_image_text(row->name, DRIVER_EXPORT_NAME_MAX, base, total))
            return false;
        if (row->params == NULL || !in_image_text(row->params, DRIVER_CALL_ARGS_MAX + 1, base, total))
            return false;
        if (row->help != NULL && !in_image_text(row->help, DRIVER_EXPORT_HELP_MAX, base, total))
            return false;
        if (row->fn == NULL || !in_code((const void*)row->fn, h, base))
            return false;
    }
}

// What kdrv_entry returned is a driver that lives in the module and is
// built for this kernel. Its code pointers must point into the module: a
// pointer that doesn't is almost always a relocation that was not applied.
static bool driver_sane(const struct Driver* drv, const KdrvHeader* h, const uint8_t* base, uint32_t total) {
    if (!in_image(drv, sizeof(*drv), base, total))
        return false;
    if (drv->abi != DRIVER_ABI || (drv->flags & DRIVER_BUILTIN) != 0 || drv->type >= DRIVER_TYPE_COUNT)
        return false;
    if (strnlen(drv->name, DRIVER_NAME_MAX) >= DRIVER_NAME_MAX ||
        strncmp(drv->name, h->name, DRIVER_NAME_MAX) != 0)
        return false;
    if (drv->probe != NULL && !in_code((const void*)drv->probe, h, base))
        return false;
    if (drv->remove != NULL && !in_code((const void*)drv->remove, h, base))
        return false;
    if (drv->poll != NULL && !in_code((const void*)drv->poll, h, base))
        return false;
    if (drv->version != NULL && !in_image_text(drv->version, 64, base, total))
        return false;

    // The match table: every row, and its end, inside the module
    if (drv->match != NULL) {
        const struct DeviceMatch* row = drv->match;
        for (uint32_t n = 0; ; n++, row++) {
            if (n > 64 || !in_image(row, sizeof(*row), base, total))
                return false;
            if (row->bus == 0)
                break;
            if (row->name != NULL && !in_image_text(row->name, DEVICE_NAME_MAX, base, total))
                return false;
        }
    }
    if (drv->exports != NULL && !exports_sane(drv->exports, h, base, total))
        return false;

    // Everything it points at is in the module: now whether it fits its type
    return driver_shape_ok(drv);
}

static KdrvModule* module_by_name(const char* name) {
    for (uint32_t i = 0; i < KDRV_LOADED_MAX; i++) {
        if (modules[i].used && strncmp(modules[i].driver->name, name, DRIVER_NAME_MAX) == 0)
            return &modules[i];
    }
    return NULL;
}

static KdrvModule* module_by_driver(const struct Driver* drv) {
    for (uint32_t i = 0; i < KDRV_LOADED_MAX; i++) {
        if (modules[i].used && modules[i].driver == drv)
            return &modules[i];
    }
    return NULL;
}

static KdrvModule* module_free_slot(void) {
    for (uint32_t i = 0; i < KDRV_LOADED_MAX; i++) {
        if (!modules[i].used)
            return &modules[i];
    }
    return NULL;
}

static void log_line(const char* what, const char* name, const char* path, KdrvResult result) {
    char line[VFS_PATH_MAX + 96];
    int length;
    if (result == KDRV_OK && path != NULL)
        length = snprintf(line, sizeof(line), "kdrv: %s %s from %s\n", what, name, path);
    else if (result == KDRV_OK)
        length = snprintf(line, sizeof(line), "kdrv: %s %s\n", what, name);
    else
        length = snprintf(line, sizeof(line), "kdrv: %s %s: %s\n", what, (path != NULL) ? path : name,
                          kdrv_error_string(result));
    if (length <= 0)
        return;
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
        line[length - 1] = '\n';
    }
    serial_write(line, (uint32_t)length);
}

static KdrvResult load_locked(const char* path, KdrvInfo* info) {
    uint8_t* file = NULL;
    uint32_t size = 0;
    KdrvResult result = read_file(path, &file, &size);
    if (result != KDRV_OK)
        return result;

    result = kdrv_check(file, size);
    if (result != KDRV_OK) {
        free(file);
        return result;
    }

    KdrvHeader h;
    memcpy(&h, file, sizeof(h));

    // The cheap refusals before any of its code runs
    if (driver_find(h.name) != NULL || module_by_name(h.name) != NULL) {
        free(file);
        return KDRV_ERR_EXISTS;
    }
    KdrvModule* slot = module_free_slot();
    if (slot == NULL || driver_count() >= DRIVER_MAX) {
        free(file);
        return KDRV_ERR_FULL;
    }

    uint32_t total = h.image_size + h.bss_size;
    uint32_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t* base = (uint8_t*)vmm_alloc_pages(pages);
    if (base == NULL) {
        free(file);
        return KDRV_ERR_NO_MEMORY;
    }

    // Image, zeroed bss and the rest of the last page, then the addresses
    memcpy(base, file + h.image_offset, h.image_size);
    memset(base + h.image_size, 0, pages * PAGE_SIZE - h.image_size);
    for (uint32_t i = 0; i < h.reloc_count; i++) {
        uint32_t at = get_u32(file + h.reloc_offset + i * 4);
        put_u32(base + at, get_u32(base + at) + (uint32_t)base);
    }
    free(file);

    KdrvEntry entry = (KdrvEntry)(base + h.entry);
    const struct Driver* drv = entry(&kernel_api);

    if (drv == NULL || !driver_sane(drv, &h, base, total)) {
        vmm_free_pages(base, pages);
        return KDRV_ERR_REFUSED;
    }

    slot->used   = true;
    slot->driver = drv;
    slot->base   = base;
    slot->pages  = pages;
    slot->calls  = 0;
    strncpy(slot->path, path, sizeof(slot->path) - 1);
    slot->path[sizeof(slot->path) - 1] = '\0';

    if (!driver_register(drv)) {
        // Lost a race for the name, or the table filled up meanwhile
        slot->used = false;
        vmm_free_pages(base, pages);
        return (driver_find(h.name) != NULL) ? KDRV_ERR_EXISTS : KDRV_ERR_FULL;
    }

    memset(info, 0, sizeof(*info));
    strncpy(info->name, drv->name, sizeof(info->name) - 1);
    if (drv->version != NULL)
        strncpy(info->version, drv->version, sizeof(info->version) - 1);
    info->image_size = total;
    info->devices    = driver_device_count(drv);
    return KDRV_OK;
}

KdrvResult kdrv_load(const char* path, KdrvInfo* info) {
    KdrvInfo loaded;
    memset(&loaded, 0, sizeof(loaded));
    mutex_lock(&kdrv_mutex);
    KdrvResult result = load_locked(path, &loaded);
    mutex_unlock(&kdrv_mutex);

    log_line(result == KDRV_OK ? "loaded" : "load", loaded.name, path, result);
    if (result == KDRV_OK && info != NULL)
        *info = loaded;
    return result;
}

KdrvResult kdrv_unload(const char* name) {
    if (name == NULL)
        return KDRV_ERR_NOT_FOUND;

    mutex_lock(&kdrv_mutex);
    KdrvModule* module = module_by_name(name);
    KdrvResult result = KDRV_OK;
    if (module == NULL) {
        const struct Driver* drv = driver_find(name);
        result = (drv != NULL && (drv->flags & DRIVER_BUILTIN)) ? KDRV_ERR_BUILTIN : KDRV_ERR_NOT_FOUND;
    } else if (module->calls != 0) {
        result = KDRV_ERR_BUSY;
    } else if (!driver_unregister(module->driver)) {
        result = KDRV_ERR_NOT_FOUND;
    } else {
        // remove() has run on every device it held and its poll has
        // returned: nothing in the kernel points into it any more
        vmm_free_pages(module->base, module->pages);
        module->used   = false;
        module->driver = NULL;
        module->base   = NULL;
    }
    mutex_unlock(&kdrv_mutex);

    log_line(result == KDRV_OK ? "unloaded" : "unload", name, NULL, result);
    return result;
}

const struct Driver* kdrv_hold(const char* name) {
    if (name == NULL)
        return NULL;
    mutex_lock(&kdrv_mutex);
    const struct Driver* drv = driver_find(name);
    if (drv != NULL) {
        KdrvModule* module = module_by_driver(drv);
        if (module != NULL)
            module->calls++;
    }
    mutex_unlock(&kdrv_mutex);
    return drv;
}

void kdrv_release(const struct Driver* drv) {
    if (drv == NULL)
        return;
    mutex_lock(&kdrv_mutex);
    KdrvModule* module = module_by_driver(drv);
    if (module != NULL && module->calls != 0)
        module->calls--;
    mutex_unlock(&kdrv_mutex);
}

bool kdrv_origin(const struct Driver* drv, char* path, uint32_t size) {
    bool found = false;
    mutex_lock(&kdrv_mutex);
    for (uint32_t i = 0; i < KDRV_LOADED_MAX && !found; i++) {
        if (modules[i].used && modules[i].driver == drv) {
            if (path != NULL && size > 0) {
                strncpy(path, modules[i].path, size - 1);
                path[size - 1] = '\0';
            }
            found = true;
        }
    }
    mutex_unlock(&kdrv_mutex);
    return found;
}
