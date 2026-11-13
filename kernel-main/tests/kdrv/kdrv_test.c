//
// Host test for the driver loader (kernel/driver/kdrv_loader.c)
//
// The real loader, compiled with the kernel's flags, linked against stubs of
// what it calls (VFS, VMM, driver core, char devices) and run on the build
// machine as a 32-bit program. It loads the real bin/drivers/hello built by
// build-drivers.sh at whatever address mmap gives, so the relocations, the
// entry point and the driver's own code all run for real.
//
//   tests/kdrv/run.sh
//

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/mman.h>

// The kernel headers' view of things, as the loader was compiled against
#include "kernel/driver/driver.h"
#include "kernel/driver/device.h"
#include "kernel/driver/chardev.h"
#include "kernel/driver/kdrv.h"
#include "kernel/driver/kapi.h"
#include "kernel/driver/kdrv_loader.h"
#include "kernel/driver/drvcall.h"

#define SYS_SEC_ADMIN 0x00000100u       // kernel/syscall/syscall.h

typedef struct { uint64_t size; uint8_t permissions, type; uint32_t policy, created, modified; } FSFileStats;
typedef uint32_t File;
struct ThreadBlock;

static int failures = 0;
static int checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

//
// Stubs: files
//

#define FILES_MAX 16
static struct { char path[64]; uint8_t* data; uint32_t size; bool denied; } files[FILES_MAX];
static struct { int file; uint32_t pos; bool open; } handles[8];

static void file_put(const char* path, const uint8_t* data, uint32_t size, bool denied) {
    for (int i = 0; i < FILES_MAX; i++) {
        if (files[i].path[0] == '\0' || strcmp(files[i].path, path) == 0) {
            strcpy(files[i].path, path);
            free(files[i].data);
            files[i].data = malloc(size ? size : 1);
            memcpy(files[i].data, data, size);
            files[i].size = size;
            files[i].denied = denied;
            return;
        }
    }
    abort();
}

static int file_find(const char* path) {
    for (int i = 0; i < FILES_MAX; i++)
        if (files[i].path[0] != '\0' && strcmp(files[i].path, path) == 0)
            return i;
    return -1;
}

bool vfs_stat(const char* path, FSFileStats* st) {
    int f = file_find(path);
    if (f < 0) return false;
    memset(st, 0, sizeof(*st));
    st->size = files[f].size;
    st->type = 0;
    return true;
}
bool vfs_check_access(const char* path) { int f = file_find(path); return f < 0 || !files[f].denied; }
File vfs_open(const char* path, uint16_t flags) {
    (void)flags;
    int f = file_find(path);
    if (f < 0 || files[f].denied) return 0;
    for (int h = 0; h < 8; h++) {
        if (!handles[h].open) { handles[h].open = true; handles[h].file = f; handles[h].pos = 0; return (File)(h + 1); }
    }
    return 0;
}
void vfs_close(File file) { handles[file - 1].open = false; }
int32_t vfs_read(File file, void* buffer, uint32_t size) {
    int f = handles[file - 1].file;
    uint32_t left = files[f].size - handles[file - 1].pos;
    if (size > left) size = left;
    if (size > 300) size = 300;         // Short reads, as a real device gives
    memcpy(buffer, files[f].data + handles[file - 1].pos, size);
    handles[file - 1].pos += size;
    return (int32_t)size;
}
uint64_t vfs_get_size(File file) { return files[handles[file - 1].file].size; }
bool vfs_file_is_device(File file, uint32_t* security) { (void)file; (void)security; return false; }

//
// Stubs: memory, locks, time, serial
//

static int pages_out = 0;
void* vmm_alloc_pages(size_t n) {
    void* p = mmap(NULL, n * 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    memset(p, 0xCC, n * 4096);           // Garbage, so a missed bss clear shows
    pages_out += (int)n;
    return p;
}
void vmm_free_pages(void* p, size_t n) { munmap(p, n * 4096); pages_out -= (int)n; }

typedef struct Mutex { bool locked; void* a; void* b; void* c; } mutex_t;
void mutex_init(mutex_t* m) { memset(m, 0, sizeof(*m)); }
void mutex_lock(mutex_t* m) { if (m->locked) { printf("FAIL: mutex taken twice\n"); abort(); } m->locked = true; }
void mutex_unlock(mutex_t* m) { m->locked = false; }

uint64_t timer_get_ms(void) { return 1234; }
uint32_t process_current_security(void) { return 0; }
void thread_sleep(uint32_t ms) { (void)ms; }

static char serial_log[4096];
void serial_write(const char* text, uint32_t length) {
    size_t used = strlen(serial_log);
    if (used + length < sizeof(serial_log)) { memcpy(serial_log + used, text, length); serial_log[used + length] = '\0'; }
    fwrite(text, 1, length, stdout);
}

//
// Stubs: devices and the driver table
//

static struct Device devices[4];
static int device_n = 0;
struct Device* device_add_platform(const char* name) {
    struct Device* d = &devices[device_n++];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, DEVICE_NAME_MAX - 1);
    d->bus = BUS_PLATFORM;
    return d;
}
struct Device* device_find(const char* name) {
    for (int i = 0; i < device_n; i++) if (strcmp(devices[i].name, name) == 0) return &devices[i];
    return NULL;
}
void device_remove(struct Device* dev) { (void)dev; }

// HDA codec drivers (KAPI 3): the loader only passes these on
struct HdaCodecDriver;
bool hda_codec_register(const struct HdaCodecDriver* d) { (void)d; return false; }
void hda_codec_unregister(const struct HdaCodecDriver* d) { (void)d; }

static const struct Driver* registered[DRIVER_MAX];
static uint32_t registered_n = 0;
static const struct Driver builtin = { .name = "memdev", .abi = DRIVER_ABI, .flags = DRIVER_BUILTIN };

static bool matches(const struct Driver* drv, const struct Device* dev) {
    if (drv->match == NULL) return false;
    for (const struct DeviceMatch* row = drv->match; row->bus != 0; row++)
        if (row->bus == dev->bus && row->name != NULL && strcmp(row->name, dev->name) == 0) return true;
    return false;
}
bool driver_register(const struct Driver* drv) {
    if (drv->abi != DRIVER_ABI || !driver_shape_ok(drv)) return false;
    for (uint32_t i = 0; i < registered_n; i++) if (strcmp(registered[i]->name, drv->name) == 0) return false;
    registered[registered_n++] = drv;
    for (int i = 0; i < device_n; i++) {
        if (devices[i].driver == NULL && matches(drv, &devices[i]) && drv->probe(&devices[i]) == PROBE_BOUND)
            devices[i].driver = drv;
    }
    return true;
}
bool driver_unregister(const struct Driver* drv) {
    for (uint32_t i = 0; i < registered_n; i++) {
        if (registered[i] != drv) continue;
        if (drv->flags & DRIVER_BUILTIN) return false;
        for (int d = 0; d < device_n; d++) {
            if (devices[d].driver == drv) { if (drv->remove) drv->remove(&devices[d]); devices[d].driver = NULL; devices[d].driver_data = NULL; }
        }
        registered[i] = registered[--registered_n];
        return true;
    }
    return false;
}
uint32_t driver_count(void) { return registered_n; }
const struct Driver* driver_get(uint32_t i) { return i < registered_n ? registered[i] : NULL; }
const struct Driver* driver_find(const char* name) {
    for (uint32_t i = 0; i < registered_n; i++) if (strcmp(registered[i]->name, name) == 0) return registered[i];
    return NULL;
}
uint32_t driver_device_count(const struct Driver* drv) {
    uint32_t n = 0;
    for (int i = 0; i < device_n; i++) if (devices[i].driver == drv) n++;
    return n;
}

//
// Stubs: character devices
//

static struct CharDevice* chardev = NULL;
struct CharDevice* chardev_register(const char* name, const struct CharDeviceOps* ops, void* data, uint32_t security) {
    if (chardev != NULL) return NULL;
    chardev = calloc(1, sizeof(*chardev));
    strncpy(chardev->name, name, CHARDEV_NAME_MAX - 1);
    chardev->ops = ops;
    chardev->driver_data = data;
    chardev->security = security;
    return chardev;
}
void chardev_unregister(struct CharDevice* cd) { if (cd != NULL && cd == chardev) { free(cd); chardev = NULL; } }
bool chardev_copy_from_user(void* d, uint32_t a, uint32_t l) { (void)d; (void)a; (void)l; return false; }
bool chardev_copy_to_user(uint32_t a, const void* s, uint32_t l) { (void)a; (void)s; (void)l; return false; }

//
// Helpers
//

static uint8_t* read_host_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) { printf("can't open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(*size);
    if (fread(data, 1, *size, f) != *size) exit(2);
    fclose(f);
    return data;
}

static void reseal(uint8_t* file, uint32_t size) {
    uint32_t zero = 0;
    memcpy(file + offsetof(KdrvHeader, checksum), &zero, 4);
    uint32_t crc = kdrv_crc32(0, file, size);
    memcpy(file + offsetof(KdrvHeader, checksum), &crc, 4);
}

// A copy of `good` with one header field changed and the CRC made right again
static KdrvResult check_with(const uint8_t* good, uint32_t size, size_t field, uint32_t value, int width) {
    uint8_t* f = malloc(size);
    memcpy(f, good, size);
    memcpy(f + field, &value, (size_t)width);
    reseal(f, size);
    KdrvResult r = kdrv_check(f, size);
    free(f);
    return r;
}

static int chardev_read(char* out, uint32_t size) {
    if (chardev->ops->open) chardev->ops->open(chardev, 1);
    int total = 0;
    for (;;) {
        int n = chardev->ops->read(chardev, out + total, size - 1 - (uint32_t)total);
        if (n <= 0) break;
        total += n;
    }
    out[total] = '\0';
    return total;
}

//
// Library drivers (drvcall.c + demolib)
//

static DrvCallResult call_lib(const char* fn, const char* types, int32_t* result, char* out, uint32_t out_size, ...) {
    struct DriverCall call;
    memset(&call, 0xAB, sizeof(call));      // Garbage in the unused slots
    va_list ap;
    va_start(ap, out_size);
    call.argc = (uint32_t)strlen(types);
    for (uint32_t i = 0; i < call.argc; i++) {
        call.args[i].type = types[i];
        if (types[i] == 's') call.args[i].s = va_arg(ap, const char*);
        else call.args[i].i = va_arg(ap, int32_t);
    }
    va_end(ap);
    call.out = out;
    call.out_size = out_size;
    return driver_call("demolib", fn, &call, 0, result);
}

static void test_library(const char* path) {
    uint32_t size;
    uint8_t* lib = read_host_file(path, &size);
    CHECK(kdrv_check(lib, size) == KDRV_OK, "demolib fails kdrv_check");
    file_put("/sys/drivers/demolib", lib, size, false);
    uint32_t hello_size;
    uint8_t* hello = read_host_file("bin/drivers/hello", &hello_size);
    file_put("/sys/drivers/hello", hello, hello_size, false);
    free(hello);

    KdrvInfo info;
    KdrvResult r = kdrv_load("/sys/drivers/demolib", &info);
    CHECK(r == KDRV_OK, "load demolib: %s", kdrv_error_string(r));
    CHECK(info.devices == 0, "a library took %u devices", info.devices);
    const struct Driver* drv = driver_find("demolib");
    CHECK(drv != NULL && drv->type == DRIVER_TYPE_LIBRARY, "demolib not a library");
    CHECK(strcmp(driver_type_name(DRIVER_TYPE_LIBRARY), "library") == 0, "type name");

    int32_t v = 0;
    char out[64];
    CHECK(call_lib("add", "ii", &v, NULL, 0, 2, 40) == DRVCALL_OK && v == 42, "add gave %d", v);
    CHECK(call_lib("sub", "ii", &v, NULL, 0, 2, 40) == DRVCALL_OK && v == -38, "sub gave %d", v);
    CHECK(call_lib("mul", "ii", &v, out, sizeof(out), -6, 7) == DRVCALL_OK && v == -42 && out[0] == 0,
          "mul gave %d '%s'", v, out);
    CHECK(call_lib("hex", "i", &v, out, sizeof(out), 255) == DRVCALL_OK && strcmp(out, "0x000000FF") == 0, "hex '%s'", out);
    CHECK(call_lib("upper", "s", &v, out, sizeof(out), "Hello there") == DRVCALL_OK &&
          strcmp(out, "HELLO THERE") == 0 && v == 11, "upper '%s' %d", out, v);
    CHECK(call_lib("reverse", "s", &v, out, sizeof(out), "abc") == DRVCALL_OK && strcmp(out, "cba") == 0, "reverse '%s'", out);
    CHECK(call_lib("crc32", "s", &v, out, sizeof(out), "123456789") == DRVCALL_OK &&
          (uint32_t)v == 0xCBF43926u && strcmp(out, "cbf43926") == 0, "crc32 %08x '%s'", (unsigned)v, out);
    CHECK(call_lib("greet", "s", &v, out, sizeof(out), "A") == DRVCALL_OK &&
          strcmp(out, "Hello, A! (from a library driver)") == 0, "greet '%s'", out);
    CHECK(call_lib("uptime", "", &v, out, sizeof(out)) == DRVCALL_OK && v == 1 && strcmp(out, "up 0d 00:00:01") == 0,
          "uptime %d '%s'", v, out);
    // A small buffer: cut and terminated, never overrun
    char small[5];
    memset(small, 'Z', sizeof(small));
    CHECK(call_lib("upper", "s", &v, small, 4, "abcdefgh") == DRVCALL_OK && strcmp(small, "ABC") == 0 &&
          small[4] == 'Z' && v == 8, "small '%.5s'", small);
    // No text wanted
    CHECK(call_lib("greet", "s", &v, NULL, 0, "x") == DRVCALL_OK, "greet without out");

    // Wrong calls never run
    int32_t before = 0, after = 0;
    call_lib("count", "", &before, NULL, 0);
    CHECK(call_lib("add", "i", &v, NULL, 0, 1) == DRVCALL_BAD_ARGS, "one arg accepted");
    CHECK(call_lib("add", "iii", &v, NULL, 0, 1, 2, 3) == DRVCALL_BAD_ARGS, "three args accepted");
    CHECK(call_lib("add", "si", &v, NULL, 0, "1", 2) == DRVCALL_BAD_ARGS, "string for int accepted");
    CHECK(call_lib("upper", "s", &v, NULL, 0, NULL) == DRVCALL_BAD_ARGS, "NULL string accepted");
    char longtext[300];
    memset(longtext, 'a', sizeof(longtext) - 1);
    longtext[sizeof(longtext) - 1] = 0;
    CHECK(call_lib("upper", "s", &v, out, sizeof(out), longtext) == DRVCALL_BAD_ARGS, "300-byte string accepted");
    CHECK(call_lib("nothere", "", &v, NULL, 0) == DRVCALL_NO_FUNCTION, "unknown function");
    CHECK(call_lib("log", "s", &v, out, sizeof(out), "hi") == DRVCALL_DENIED, "admin-only function ran");
    call_lib("count", "", &after, NULL, 0);
    CHECK(after == before + 1, "refused calls ran: count %d -> %d", before, after);

    struct DriverCall call = { .argc = 1 };
    call.args[0].type = 's';
    call.args[0].s = "from the test";
    CHECK(driver_call("demolib", "log", &call, SYS_SEC_ADMIN, &v) == DRVCALL_OK, "admin log refused");
    CHECK(strstr(serial_log, "demolib: from the test") != NULL, "log line missing");
    CHECK(driver_call("nolib", "add", &call, DRVCALL_TRUSTED, &v) == DRVCALL_NO_DRIVER, "unknown driver");

    // hello is a device driver: nothing to call
    CHECK(kdrv_load("/sys/drivers/hello", NULL) == KDRV_OK, "load hello next to demolib");
    call.argc = 0;
    CHECK(driver_call("hello", "add", &call, DRVCALL_TRUSTED, &v) == DRVCALL_NOT_LIBRARY, "hello callable");

    // Listing
    DrvExportInfo e;
    CHECK(driver_export_info("demolib", 0, &e) == DRVCALL_OK && strcmp(e.name, "add") == 0 &&
          strcmp(e.params, "ii") == 0 && strcmp(e.help, "a + b") == 0, "export 0");
    CHECK(driver_export_info("demolib", 11, &e) == DRVCALL_OK && strcmp(e.name, "log") == 0 &&
          e.security == SYS_SEC_ADMIN, "export 11");
    CHECK(driver_export_info("demolib", 12, &e) == DRVCALL_NO_FUNCTION, "export past the end");
    CHECK(driver_export_info("hello", 0, &e) == DRVCALL_NOT_LIBRARY, "hello exports");

    // No unload while a call holds it
    const struct Driver* held = kdrv_hold("demolib");
    CHECK(held == drv, "hold");
    CHECK(kdrv_unload("demolib") == KDRV_ERR_BUSY, "unloaded during a call");
    kdrv_release(held);
    CHECK(kdrv_unload("demolib") == KDRV_OK, "unload demolib");
    CHECK(kdrv_unload("hello") == KDRV_OK, "unload hello");
    CHECK(call_lib("add", "ii", &v, NULL, 0, 1, 2) == DRVCALL_NO_DRIVER, "call after unload");
    CHECK(pages_out == 0, "pages left %d", pages_out);

    // Twice in a row: state starts clean
    CHECK(kdrv_load("/sys/drivers/demolib", NULL) == KDRV_OK, "reload demolib");
    CHECK(call_lib("count", "", &v, NULL, 0) == DRVCALL_OK && v == 1, "count after reload %d", v);
    CHECK(kdrv_unload("demolib") == KDRV_OK, "unload demolib 2");

    // A library file whose table is broken is refused after its entry runs
    // (rename "upper" to "add": two functions of one name)
    uint32_t lsize;
    uint8_t* copy = read_host_file(path, &lsize);
    uint8_t* bad = copy;
    KdrvHeader h;
    memcpy(&h, bad, sizeof(h));
    uint8_t* up = memmem(bad + h.image_offset, h.image_size, "upper\0", 6);
    CHECK(up != NULL, "name not in the image");
    if (up != NULL) {
        memcpy(up, "add\0\0", 6);
        uint32_t zero = 0;
        memcpy(bad + offsetof(KdrvHeader, checksum), &zero, 4);
        uint32_t crc = kdrv_crc32(0, bad, lsize);
        memcpy(bad + offsetof(KdrvHeader, checksum), &crc, 4);
        file_put("/duplib", bad, lsize, false);
        CHECK(kdrv_load("/duplib", NULL) == KDRV_ERR_REFUSED, "duplicate export names accepted");
        CHECK(pages_out == 0 && driver_find("demolib") == NULL, "refused library left state");
    }
    free(copy);
}

static int32_t shape_fn(struct DriverCall* c) { (void)c; return 0; }
static int shape_probe(struct Device* d) { (void)d; return PROBE_NOT_MINE; }
static void shape_poll(void) {}

static void test_shapes(void) {
    static const struct DeviceMatch m[] = { { .bus = BUS_PLATFORM, .name = "x" }, { 0 } };
    static const struct DriverExport good[] = { { "a", "is", NULL, 0, shape_fn }, { "b-2_c", "", "h", 0, shape_fn }, { 0 } };
    static const struct DriverExport dup[]  = { { "a", "", NULL, 0, shape_fn }, { "a", "", NULL, 0, shape_fn }, { 0 } };
    static const struct DriverExport letter[] = { { "a", "ix", NULL, 0, shape_fn }, { 0 } };
    static const struct DriverExport many[] = { { "a", "iiiiiiiii", NULL, 0, shape_fn }, { 0 } };
    static const struct DriverExport space[] = { { "a b", "", NULL, 0, shape_fn }, { 0 } };
    static const struct DriverExport nofn[] = { { "a", "", NULL, 0, NULL }, { 0 } };
    static const struct DriverExport empty[] = { { 0 } };

    struct Driver d = { .name = "t", .abi = DRIVER_ABI };
    d.type = DRIVER_TYPE_DEVICE; d.match = m; d.probe = shape_probe;
    CHECK(driver_shape_ok(&d), "device");
    d.exports = good;
    CHECK(!driver_shape_ok(&d), "device with exports");
    d.exports = NULL; d.match = NULL;
    CHECK(!driver_shape_ok(&d), "device without match");

    d = (struct Driver){ .name = "t", .abi = DRIVER_ABI, .type = DRIVER_TYPE_LIBRARY, .exports = good };
    CHECK(driver_shape_ok(&d), "library");
    d.poll = shape_poll;
    CHECK(driver_shape_ok(&d), "library with poll");
    d.probe = shape_probe;
    CHECK(!driver_shape_ok(&d), "library with probe");
    d.probe = NULL;
    d.exports = dup;    CHECK(!driver_shape_ok(&d), "duplicate names");
    d.exports = letter; CHECK(!driver_shape_ok(&d), "bad param letter");
    d.exports = many;   CHECK(!driver_shape_ok(&d), "9 params");
    d.exports = space;  CHECK(!driver_shape_ok(&d), "space in name");
    d.exports = nofn;   CHECK(!driver_shape_ok(&d), "no function");
    d.exports = empty;  CHECK(!driver_shape_ok(&d), "empty table");
    d.exports = NULL;   CHECK(!driver_shape_ok(&d), "no table");

    d = (struct Driver){ .name = "t", .abi = DRIVER_ABI, .type = DRIVER_TYPE_SERVICE, .poll = shape_poll };
    CHECK(driver_shape_ok(&d), "service");
    d.poll = NULL;
    CHECK(!driver_shape_ok(&d), "service without poll");
    d.type = 7;
    CHECK(!driver_shape_ok(&d), "unknown type");
}

int main(int argc, char** argv) {
    const char* driver_path = (argc > 1) ? argv[1] : "bin/drivers/hello";
    uint32_t size;
    uint8_t* hello = read_host_file(driver_path, &size);

    kdrv_loader_init();
    registered[registered_n++] = &builtin;

    // The file as built
    CHECK(kdrv_check(hello, size) == KDRV_OK, "built driver file fails kdrv_check");
    CHECK(memcmp(hello, "kdrv", 4) == 0, "no magic");

    // Load and use it
    file_put("/sys/drivers/hello", hello, size, false);
    KdrvInfo info;
    KdrvResult r = kdrv_load("/sys/drivers/hello", &info);
    CHECK(r == KDRV_OK, "load: %s", kdrv_error_string(r));
    CHECK(strcmp(info.name, "hello") == 0 && strcmp(info.version, "1.0") == 0, "info %s %s", info.name, info.version);
    CHECK(info.devices == 1, "devices %u", info.devices);
    CHECK(chardev != NULL && strcmp(chardev->name, "hello") == 0, "no /dev/hello");
    CHECK(pages_out == 1, "pages %d", pages_out);
    char path[64];
    CHECK(kdrv_origin(driver_find("hello"), path, sizeof(path)) && strcmp(path, "/sys/drivers/hello") == 0, "origin");
    CHECK(!kdrv_origin(&builtin, NULL, 0), "builtin has an origin");

    char text[128];
    if (chardev != NULL) {
        chardev_read(text, sizeof(text));
        CHECK(strcmp(text, "Hello from a driver loaded off the disk!\n") == 0, "read gave '%s'", text);
        chardev->ops->write(chardev, "changed\n", 8);
        chardev_read(text, sizeof(text));
        CHECK(strcmp(text, "changed\n") == 0, "after write: '%s'", text);
    }

    // Twice: refused before any of it runs
    CHECK(kdrv_load("/sys/drivers/hello", NULL) == KDRV_ERR_EXISTS, "second load not EXISTS");
    CHECK(pages_out == 1, "second load leaked pages");

    // Unload, and the bad unloads
    CHECK(kdrv_unload("memdev") == KDRV_ERR_BUILTIN, "unload builtin");
    CHECK(kdrv_unload("nothere") == KDRV_ERR_NOT_FOUND, "unload unknown");
    CHECK(kdrv_unload("hello") == KDRV_OK, "unload");
    CHECK(chardev == NULL, "/dev/hello left behind");
    CHECK(pages_out == 0, "pages left %d", pages_out);
    CHECK(driver_find("hello") == NULL, "still registered");
    CHECK(kdrv_unload("hello") == KDRV_ERR_NOT_FOUND, "unload twice");

    // Again: the platform device is reused, the bss starts clean
    CHECK(kdrv_load("/sys/drivers/hello", &info) == KDRV_OK, "reload");
    CHECK(device_n == 1, "devices added %d", device_n);
    if (chardev != NULL) {
        chardev_read(text, sizeof(text));
        CHECK(strcmp(text, "Hello from a driver loaded off the disk!\n") == 0, "reload read gave '%s'", text);
    }
    CHECK(kdrv_unload("hello") == KDRV_OK, "unload 2");

    // Files that aren't drivers, or are damaged
    const uint8_t elf[] = { 0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    file_put("/bin/prog", elf, sizeof(elf), false);
    CHECK(kdrv_load("/bin/prog", NULL) == KDRV_ERR_NOT_DRIVER, "ELF accepted");
    file_put("/empty", elf, 0, false);
    CHECK(kdrv_load("/empty", NULL) == KDRV_ERR_NOT_DRIVER, "empty accepted");
    CHECK(kdrv_load("/missing", NULL) == KDRV_ERR_NOT_FOUND, "missing file");
    file_put("/secret", hello, size, true);
    CHECK(kdrv_load("/secret", NULL) == KDRV_ERR_ACCESS, "denied file");
    file_put("/short", hello, 40, false);
    CHECK(kdrv_load("/short", NULL) == KDRV_ERR_BAD_FORMAT, "short header");
    file_put("/trunc", hello, size - 8, false);
    r = kdrv_load("/trunc", NULL);
    CHECK(r == KDRV_ERR_BAD_FORMAT || r == KDRV_ERR_CHECKSUM, "truncated: %s", kdrv_error_string(r));

    uint8_t* bad = malloc(size);
    memcpy(bad, hello, size);
    bad[sizeof(KdrvHeader) + 20] ^= 0x01;
    CHECK(kdrv_check(bad, size) == KDRV_ERR_CHECKSUM, "flipped bit not caught");
    file_put("/flipped", bad, size, false);
    CHECK(kdrv_load("/flipped", NULL) == KDRV_ERR_CHECKSUM, "flipped load");

    CHECK(check_with(hello, size, offsetof(KdrvHeader, machine), 62, 2) == KDRV_ERR_MISMATCH, "x86-64 accepted");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, driver_abi), DRIVER_ABI + 1, 2) == KDRV_ERR_MISMATCH, "abi");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, kapi_version), KAPI_VERSION + 1, 4) == KDRV_ERR_MISMATCH, "kapi newer");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, version), 2, 2) == KDRV_ERR_BAD_FORMAT, "version 2");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, flags), 1, 4) == KDRV_ERR_BAD_FORMAT, "flags");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, entry), 0x7FFFFFFF, 4) == KDRV_ERR_BAD_FORMAT, "entry");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, image_size), 0xFFFFFFF0, 4) == KDRV_ERR_TOO_BIG, "image size");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, bss_size), 0xFFFFFFF0, 4) == KDRV_ERR_TOO_BIG, "bss size");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, image_offset), 0, 4) == KDRV_ERR_BAD_FORMAT, "image over header");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, reloc_count), 0x40000001, 4) == KDRV_ERR_BAD_FORMAT, "reloc count wrap");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, reloc_offset), 2, 4) == KDRV_ERR_BAD_FORMAT, "reloc unaligned");
    CHECK(check_with(hello, size, offsetof(KdrvHeader, name), 0, 1) == KDRV_ERR_BAD_FORMAT, "empty name");

    // A relocation that points past the image
    {
        KdrvHeader h;
        memcpy(&h, hello, sizeof(h));
        memcpy(bad, hello, size);
        uint32_t past = h.image_size - 2;
        memcpy(bad + h.reloc_offset, &past, 4);
        reseal(bad, size);
        CHECK(kdrv_check(bad, size) == KDRV_ERR_BAD_FORMAT, "reloc past the image");
    }

    // Header name not the driver's own: the code runs, then is refused and freed
    memcpy(bad, hello, size);
    memcpy(bad + offsetof(KdrvHeader, name), "hellx", 5);
    reseal(bad, size);
    file_put("/renamed", bad, size, false);
    CHECK(kdrv_load("/renamed", NULL) == KDRV_ERR_REFUSED, "name mismatch accepted");
    CHECK(pages_out == 0 && chardev == NULL && driver_find("hello") == NULL, "refused load left state");

    // Header fuzz: random fields with a correct CRC never crash the checker
    srand(7);
    int fuzz_ok = 0;
    for (int i = 0; i < 200000; i++) {
        memcpy(bad, hello, size);
        int n = 1 + rand() % 3;
        for (int k = 0; k < n; k++) {
            uint32_t at = (uint32_t)(rand() % 48);
            uint32_t v = (rand() & 1) ? (uint32_t)rand() * 2654435761u : (uint32_t)(rand() % 2048);
            memcpy(bad + (at & ~3u), &v, 4);
        }
        memcpy(bad, "kdrv", 4);
        reseal(bad, size);
        if (kdrv_check(bad, size) == KDRV_OK) fuzz_ok++;
    }
    printf("fuzz: 200000 mutated headers checked, %d still valid\n", fuzz_ok);

    if (argc > 2)
        test_library(argv[2]);
    test_shapes();

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
