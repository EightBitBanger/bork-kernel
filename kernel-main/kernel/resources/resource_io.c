
#include <stdint.h>
#include <stdbool.h>

#include <kernel/resources/resource_io.h>
#include <kernel/vfs/vfs.h>

bool resource_write_exact(File file, const void* buffer, uint32_t size) {
    const uint8_t* p = (const uint8_t*)buffer;
    while (size > 0) {
        int32_t n = vfs_write(file, p, size);
        if (n <= 0) return false;
        p += n;
        size -= (uint32_t)n;
    }
    return true;
}

bool resource_read_exact(File file, void* buffer, uint32_t size) {
    uint8_t* p = (uint8_t*)buffer;
    while (size > 0) {
        int32_t n = vfs_read(file, p, size);
        if (n <= 0) return false;
        p += n;
        size -= (uint32_t)n;
    }
    return true;
}

bool resource_read_at(File file, uint32_t offset, void* buffer, uint32_t size) {
    vfs_seek(file, offset);
    if (vfs_tell(file) != offset) return false;
    return resource_read_exact(file, buffer, size);
}
