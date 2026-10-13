#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/resources/resource_manager.h>
#include <kernel/resources/resource_class.h>
#include <kernel/resources/resource_io.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

// Classes
#include <kernel/resources/classes/image.h>

#ifdef KERNEL_PLATFORM_X86 
  #include <kernel/arch/x86/heap.h>
#endif

#define RESOURCE_HEADER_SIZE    16u
#define RESOURCE_MAX_PAYLOAD    (16u * 1024u * 1024u)   // Sanity cap against corrupt headers

static const ResourceHandler* const handlers[RESOURCE_TYPE_COUNT] = {
    [RESOURCE_TYPE_SPRITE] = &resource_class_image,
};

static const ResourceHandler* get_handler(ResourceType type) {
    if (type <= RESOURCE_TYPE_NONE || type >= RESOURCE_TYPE_COUNT) return NULL;
    const ResourceHandler* handler = handlers[type];
    if (handler == NULL || handler->payload_size == NULL ||
        handler->write == NULL || handler->read == NULL) return NULL;
    return handler;
}

// ==========================================
// Header
// ==========================================

// On success the file is positioned at the first payload byte
static bool read_header(File file, ResourceType* out_type, uint32_t* out_payload) {
    uint32_t file_size = vfs_get_size(file);
    if (file_size < RESOURCE_HEADER_SIZE) return false;

    uint8_t header[RESOURCE_HEADER_SIZE];
    if (!resource_read_at(file, 0, header, RESOURCE_HEADER_SIZE)) return false;

    if (memcmp(header, RESOURCE_MAGIC, 4) != 0) return false;
    if (resource_get_u16(&header[4]) != RESOURCE_FORMAT_VERSION) return false;

    uint32_t payload = resource_get_u32(&header[8]);
    if (payload > RESOURCE_MAX_PAYLOAD) return false;
    if (file_size - RESOURCE_HEADER_SIZE < payload) return false;   // Truncated file

    *out_type    = (ResourceType)resource_get_u16(&header[6]);
    *out_payload = payload;
    return true;
}

// ==========================================
// Public interface
// ==========================================

bool resource_save(const char* path, ResourceType type, const void* resource) {
    if (path == NULL || path[0] == '\0' || resource == NULL) return false;

    const ResourceHandler* handler = get_handler(type);
    if (handler == NULL) return false;

    uint32_t payload = handler->payload_size(resource);
    if (payload == 0 || payload > RESOURCE_MAX_PAYLOAD) return false;
    uint32_t total_size = RESOURCE_HEADER_SIZE + payload;

    // Make sure the file exists (CREATE only creates it when it is missing)
    File file = vfs_open(path, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) return false;
    vfs_close(file);

    // Size the file before writing: writes don't reliably extend a file, and
    // this also drops stale bytes when overwriting a larger resource
    if (!vfs_truncate(path, total_size)) return false;

    file = vfs_open(path, VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) return false;
    vfs_seek(file, 0);

    uint8_t header[RESOURCE_HEADER_SIZE];
    memcpy(header, RESOURCE_MAGIC, 4);
    resource_put_u16(&header[4], RESOURCE_FORMAT_VERSION);
    resource_put_u16(&header[6], (uint16_t)type);
    resource_put_u32(&header[8], payload);
    resource_put_u32(&header[12], 0);

    bool ok = resource_write_exact(file, header, RESOURCE_HEADER_SIZE) &&
              handler->write(file, resource);

    uint32_t final_size = vfs_get_size(file);
    vfs_close(file);

    return ok && final_size == total_size;
}

void* resource_load(const char* path, ResourceType expected_type, ResourceType* out_type) {
    if (out_type) *out_type = RESOURCE_TYPE_NONE;
    if (path == NULL || path[0] == '\0') return NULL;

    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) return NULL;

    void* resource = NULL;
    ResourceType type = RESOURCE_TYPE_NONE;
    uint32_t payload = 0;

    if (read_header(file, &type, &payload) &&
        (expected_type == RESOURCE_TYPE_NONE || expected_type == type)) {
        const ResourceHandler* handler = get_handler(type);
        if (handler != NULL) {
            resource = handler->read(file, payload);
        }
    }

    vfs_close(file);

    if (resource != NULL && out_type) *out_type = type;
    return resource;
}

bool resource_peek_type(const char* path, ResourceType* out_type) {
    if (path == NULL || path[0] == '\0' || out_type == NULL) return false;

    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) return false;

    ResourceType type = RESOURCE_TYPE_NONE;
    uint32_t payload = 0;
    bool ok = read_header(file, &type, &payload);
    vfs_close(file);

    if (!ok) return false;
    *out_type = type;
    return true;
}

void resource_free(ResourceType type, void* resource) {
    if (resource == NULL) return;
    const ResourceHandler* handler = get_handler(type);
    if (handler != NULL && handler->release != NULL) {
        handler->release(resource);
    } else {
        free(resource);
    }
}
