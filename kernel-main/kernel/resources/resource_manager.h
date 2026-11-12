#ifndef RESOURCE_MAN_H
#define RESOURCE_MAN_H

// Resource manager
//
// The resource files start with the same 16-byte header, followed by a
// type-specific payload. All multi-byte fields are little-endian and there
// is no padding anywhere.
//
//   Resource header (16 bytes)
//     0   char[4]  magic          "RSRC"
//     4   u16      version        RESOURCE_FORMAT_VERSION
//     6   u16      type           ResourceType
//     8   u32      payload_size   number of bytes following the header
//    12   u32      reserved       0
//
// Payload layouts are documented by each class in resources/classes/
// (e.g. classes/image.h for RESOURCE_TYPE_SPRITE).

#include <stdint.h>
#include <stdbool.h>

#define RESOURCE_MAGIC              "RSRC"
#define RESOURCE_FORMAT_VERSION     1

typedef enum {
    RESOURCE_TYPE_NONE   = 0,   // Invalid / "any type" when loading
    RESOURCE_TYPE_SPRITE = 1,   // classes/image.h
    
    RESOURCE_TYPE_COUNT
} ResourceType;

// Serialize a resource of the given type to path (created or overwritten)
bool resource_save(const char* path, ResourceType type, const void* resource);

// Load a resource from path. Pass RESOURCE_TYPE_NONE as expected_type to accept
// any type; out_type (may be NULL) receives the type that was loaded.
// The result is heap allocated: release it with resource_free.
void* resource_load(const char* path, ResourceType expected_type, ResourceType* out_type);

// Read only the header of a resource file to find out what it holds
bool resource_peek_type(const char* path, ResourceType* out_type);

// Release a resource returned by resource_load
void resource_free(ResourceType type, void* resource);

#endif
