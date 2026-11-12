/*
 * Functional tests, linked like a real user program (crt0 + user.ld +
 * libc) and run on Linux through linux_shim.c. Runs inside an empty
 * scratch directory.
 */

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <os/os.h>
#include <os/security.h>
#include <os/filetype.h>

int malloc_check(void);

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); } \
} while (0)

//
// malloc
//

static uint32_t rng = 12345;
static uint32_t next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void fill(unsigned char* p, size_t n, unsigned tag) {
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag + i * 7);
}
static int verify(const unsigned char* p, size_t n, unsigned tag) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(tag + i * 7)) return 0;
    return 1;
}

static void test_malloc(void) {
    enum { SLOTS = 512, ROUNDS = 40000 };
    static unsigned char* ptr[SLOTS];
    static size_t size[SLOTS];
    static unsigned tag[SLOTS];
    int bad_data = 0, bad_align = 0, bad_heap = 0;

    void* brk_start = sbrk(0);

    for (int round = 0; round < ROUNDS; round++) {
        int i = (int)(next_rand() % SLOTS);
        uint32_t op = next_rand() % 10;

        if (ptr[i] && !verify(ptr[i], size[i], tag[i])) bad_data++;

        if (ptr[i] == NULL || op < 3) {
            free(ptr[i]);
            // Mostly small, sometimes big (forces growth and trimming)
            uint32_t r = next_rand();
            size_t n = (r % 16 == 0) ? (r % 300000) : (r % 16 == 1) ? (r % 5000) : (r % 300);
            unsigned align = 0;
            if (next_rand() % 20 == 0) align = 1u << (5 + next_rand() % 8);   // 32..4096
            ptr[i] = align ? memalign(align, n) : malloc(n);
            if (ptr[i] == NULL && n > 0) { bad_heap++; continue; }
            if (((uintptr_t)ptr[i] & 15) || (align && ((uintptr_t)ptr[i] & (align - 1)))) bad_align++;
            if (ptr[i] && malloc_usable_size(ptr[i]) < n) bad_heap++;
            size[i] = n;
            tag[i] = next_rand();
            if (ptr[i]) fill(ptr[i], n, tag[i]);
        } else if (op < 6) {
            size_t n = next_rand() % 2000;
            if (next_rand() % 30 == 0) n = next_rand() % 200000;
            unsigned char* q = realloc(ptr[i], n);
            if (n == 0) { ptr[i] = NULL; size[i] = 0; continue; }
            if (q == NULL) { bad_heap++; continue; }
            size_t keep = size[i] < n ? size[i] : n;
            if (!verify(q, keep, tag[i])) bad_data++;
            ptr[i] = q;
            size[i] = n;
            fill(q, n, tag[i]);
        } else if (op < 7) {
            free(ptr[i]);
            ptr[i] = NULL;
        }

        if (round % 997 == 0 && malloc_check() != 0) bad_heap++;
    }

    CHECK(bad_data == 0);
    CHECK(bad_align == 0);
    CHECK(bad_heap == 0);
    CHECK(malloc_check() == 0);

    char* peak = sbrk(0);
    for (int i = 0; i < SLOTS; i++) { free(ptr[i]); ptr[i] = NULL; }
    CHECK(malloc_check() == 0);
    char* after = sbrk(0);
    printf("malloc: %d rounds, heap peaked at %u KB, %u KB after freeing everything\n",
           ROUNDS, (unsigned)((peak - (char*)brk_start) / 1024), (unsigned)((after - (char*)brk_start) / 1024));
    CHECK(after - (char*)brk_start <= 256 * 1024);     // Top was trimmed back

    // calloc zeroes reused memory; overflow is refused
    char* a = malloc(1000);
    memset(a, 0xAB, 1000);
    free(a);
    unsigned char* z = calloc(250, 4);
    int nonzero = 0;
    for (int i = 0; i < 1000; i++) nonzero |= z[i];
    CHECK(nonzero == 0);
    free(z);
    errno = 0;
    CHECK(calloc(SIZE_MAX / 2, 4) == NULL && errno == ENOMEM);
    CHECK(malloc(SIZE_MAX) == NULL);
    void* pm = NULL;
    CHECK(posix_memalign(&pm, 64, 100) == 0 && ((uintptr_t)pm & 63) == 0);
    free(pm);
    CHECK(posix_memalign(&pm, 3, 100) == EINVAL);

    // In-place growth at the top of the heap
    char* g = malloc(100);
    strcpy(g, "keep me");
    char* g2 = realloc(g, 50000);
    CHECK(g2 != NULL && strcmp(g2, "keep me") == 0);
    free(g2);
    CHECK(malloc_check() == 0);

    // A foreign sbrk in the middle: the heap must continue past it
    void* hold = malloc(64);
    void* foreign = sbrk(8192);
    CHECK(foreign != (void*)-1);
    memset(foreign, 0x5A, 8192);
    void* bigs[4];
    for (int i = 0; i < 4; i++) { bigs[i] = malloc(100000); CHECK(bigs[i] != NULL); memset(bigs[i], i, 100000); }
    CHECK(((unsigned char*)foreign)[8191] == 0x5A);
    CHECK(malloc_check() == 0);
    for (int i = 0; i < 4; i++) free(bigs[i]);
    free(hold);
    CHECK(malloc_check() == 0);
}

//
// strings, ctype, stdlib
//

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static void test_misc(void) {
    char buf[64];
    CHECK(strlen("hello") == 5);
    CHECK(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("x", "x") == 0);
    CHECK(strncmp("abcX", "abcY", 3) == 0);
    CHECK(strcasecmp("HeLLo", "hello") == 0);
    CHECK(strcpy(buf, "abc") == buf && strcat(buf, "def") == buf && strcmp(buf, "abcdef") == 0);
    CHECK(strchr("abcabc", 'c') - "abcabc" >= 0);
    CHECK(strrchr("a/b/c", '/') && strcmp(strrchr("a/b/c", '/'), "/c") == 0);
    CHECK(strstr("hello world", "o w") && strstr("abc", "") && !strstr("abc", "abcd"));
    CHECK(strspn("aabbcc", "ab") == 4 && strcspn("hello", "lo") == 2);
    CHECK(strlcpy(buf, "truncate me", 5) == 11 && strcmp(buf, "trun") == 0);

    // memmove both directions, odd sizes (exercises the rep movs paths)
    char m[32] = "0123456789abcdefghijklmnopqrstu";
    memmove(m + 3, m, 10);
    CHECK(memcmp(m, "0120123456789defghij", 20) == 0);
    memmove(m, m + 5, 7);
    CHECK(memcmp(m, "2345678", 7) == 0);
    memset(buf, 'x', 13); buf[13] = 0;
    CHECK(strcmp(buf, "xxxxxxxxxxxxx") == 0);

    char tok[] = "  one, two,,three ";
    char* save; char* t = strtok_r(tok, " ,", &save);
    CHECK(t && strcmp(t, "one") == 0);
    t = strtok_r(NULL, " ,", &save); CHECK(t && strcmp(t, "two") == 0);
    t = strtok_r(NULL, " ,", &save); CHECK(t && strcmp(t, "three") == 0);
    CHECK(strtok_r(NULL, " ,", &save) == NULL);

    char* d = strdup("dup");
    CHECK(d && strcmp(d, "dup") == 0);
    free(d);

    CHECK(isdigit('5') && !isdigit('a') && isspace('\t') && toupper('q') == 'Q' && isxdigit('F') && !isxdigit('g'));
    CHECK(atoi("  -42x") == -42 && labs(-7) == 7);
    errno = 0;
    CHECK(strtol("99999999999", NULL, 10) == LONG_MAX && errno == ERANGE);

    int arr[1000];
    for (int i = 0; i < 1000; i++) arr[i] = (int)(next_rand() % 500) - 250;
    qsort(arr, 1000, sizeof(int), cmp_int);
    int sorted = 1;
    for (int i = 1; i < 1000; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    CHECK(sorted);
    int key = arr[617];
    int* found = bsearch(&key, arr, 1000, sizeof(int), cmp_int);
    CHECK(found && *found == key);
    int missing = 1000;
    CHECK(bsearch(&missing, arr, 1000, sizeof(int), cmp_int) == NULL);

    // 64-bit division helpers
    volatile unsigned long long big = 0xFEDCBA9876543210ULL, div = 0x12345ULL;
    CHECK(big / div == 0xE0004FA01C4DULL && big % div == 0x10A4FULL);
    volatile long long sn = -1000000000000LL;
    CHECK(sn / 7 == -142857142857LL && sn % 7 == -1);

    char* s = NULL;
    CHECK(asprintf(&s, "%s-%04d-%.2f", "id", 42, 2.5) == 12 && strcmp(s, "id-0042-2.50") == 0);
    free(s);
}

//
// Files and stdio
//

static void test_files(void) {
    // Raw descriptors
    int fd = open("raw.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 3);
    CHECK(write(fd, "0123456789", 10) == 10);
    CHECK(lseek(fd, 2, SEEK_SET) == 2);
    char b[16] = {0};
    CHECK(read(fd, b, 4) == 4 && memcmp(b, "2345", 4) == 0);
    CHECK(lseek(fd, 0, SEEK_END) == 10);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 10 && S_ISREG(st.st_mode));
    CHECK(close(fd) == 0);
    CHECK(close(fd) == -1 && errno == EBADF);
    CHECK(open("raw.bin", O_WRONLY | O_CREAT | O_EXCL) == -1 && errno == EEXIST);
    CHECK(open("missing.txt", O_RDONLY) == -1 && errno == ENOENT);

    // Line-oriented text through stdio
    FILE* f = fopen("text.txt", "w");
    CHECK(f != NULL);
    for (int i = 0; i < 2000; i++)
        fprintf(f, "line %d: %s\n", i, (i % 3) ? "fizz" : "buzz");
    fputs("last line without newline", f);
    CHECK(fclose(f) == 0);

    CHECK(stat("text.txt", &st) == 0 && st.st_size > 20000);

    f = fopen("text.txt", "r");
    CHECK(f != NULL);
    char line[128];
    int count = 0, ok = 1;
    while (fgets(line, sizeof line, f)) {
        if (count < 2000) {
            char expect[64];
            snprintf(expect, sizeof expect, "line %d: %s\n", count, (count % 3) ? "fizz" : "buzz");
            if (strcmp(line, expect) != 0) ok = 0;
        }
        count++;
    }
    CHECK(ok && count == 2001);
    CHECK(strcmp(line, "last line without newline") == 0);
    CHECK(feof(f) && !ferror(f));
    rewind(f);
    CHECK(!feof(f) && fgetc(f) == 'l');
    CHECK(ungetc('L', f) == 'L' && fgetc(f) == 'L' && fgetc(f) == 'i');
    CHECK(ftell(f) == 2);
    fclose(f);

    // getline
    f = fopen("text.txt", "r");
    char* gl = NULL;
    size_t cap = 0;
    ssize_t len = getline(&gl, &cap, f);
    CHECK(len == 13 && strcmp(gl, "line 0: buzz\n") == 0);
    free(gl);
    fclose(f);

    // Binary blocks, mixed read/write/seek on "r+"
    f = fopen("data.bin", "w+");
    CHECK(f != NULL);
    static int nums[5000];
    for (int i = 0; i < 5000; i++) nums[i] = i * i;
    CHECK(fwrite(nums, sizeof(int), 5000, f) == 5000);
    CHECK(ftell(f) == 20000);
    CHECK(fseek(f, 400, SEEK_SET) == 0);
    int v = 0;
    CHECK(fread(&v, sizeof v, 1, f) == 1 && v == 100 * 100);
    int marker = -7;
    CHECK(fwrite(&marker, sizeof marker, 1, f) == 1);     // At 404, after a read
    CHECK(fseek(f, -8, SEEK_CUR) == 0 && ftell(f) == 400);
    int two[2];
    CHECK(fread(two, sizeof(int), 2, f) == 2 && two[0] == 10000 && two[1] == -7);
    CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 20000);
    CHECK(fread(&v, sizeof v, 1, f) == 0 && feof(f));
    fclose(f);

    f = fopen("data.bin", "rb");
    static int back[5000];
    CHECK(fread(back, sizeof(int), 5000, f) == 5000);
    nums[101] = -7;
    CHECK(memcmp(back, nums, sizeof nums) == 0);
    fclose(f);

    // Append mode
    f = fopen("log.txt", "a");
    fputs("one\n", f);
    fclose(f);
    f = fopen("log.txt", "a+");
    fputs("two\n", f);
    rewind(f);
    CHECK(fgets(line, sizeof line, f) && strcmp(line, "one\n") == 0);
    fclose(f);
    CHECK(stat("log.txt", &st) == 0 && st.st_size == 8);

    // fopen errors and modes
    CHECK(fopen("nope/x.txt", "r") == NULL && errno == ENOENT);
    CHECK(fopen("log.txt", "q") == NULL && errno == EINVAL);
    CHECK(fopen("log.txt", "wx") == NULL && errno == EEXIST);

    // stdin has no source yet
    CHECK(fgetc(stdin) == EOF && ferror(stdin));
    clearerr(stdin);

    // dprintf / fdopen
    fd = open("fd.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(dprintf(fd, "%s %d", "dprintf", 7) == 9);
    close(fd);
    fd = open("fd.txt", O_RDONLY);
    f = fdopen(fd, "r");
    CHECK(f && fgets(line, sizeof line, f) && strcmp(line, "dprintf 7") == 0);
    fclose(f);

    // truncate, access, remove
    CHECK(truncate("fd.txt", 3) == 0 && stat("fd.txt", &st) == 0 && st.st_size == 3);
    CHECK(access("fd.txt", R_OK | W_OK) == 0);
    CHECK(access("absent", F_OK) == -1 && errno == ENOENT);
    CHECK(remove("fd.txt") == 0 && access("fd.txt", F_OK) == -1);
}

//
// Directories and paths
//

static int dir_has(const char* path, const char* name, unsigned char type) {
    DIR* d = opendir(path);
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, name) == 0 && e->d_type == type) found = 1;
    closedir(d);
    return found;
}

static void test_dirs(void) {
    char cwd[512], abs[512];
    CHECK(getcwd(cwd, sizeof cwd) != NULL && cwd[0] == '/');
    CHECK(getcwd(cwd, 2) == NULL && errno == ERANGE);
    getcwd(cwd, sizeof cwd);

    CHECK(mkdir("sub", 0755) == 0);
    CHECK(mkdir("sub", 0755) == -1 && errno == EEXIST);
    CHECK(mkdir("sub/inner", 0755) == 0);
    FILE* f = fopen("sub/inner/file.txt", "w");
    fputs("x", f);
    fclose(f);

    CHECK(dir_has(".", "sub", DT_DIR));
    CHECK(dir_has("sub/inner", "file.txt", DT_REG));

    DIR* d = opendir("sub");
    int n = 0;
    while (readdir(d)) n++;
    CHECK(n == 1);
    rewinddir(d);
    CHECK(readdir(d) != NULL);
    closedir(d);
    CHECK(opendir("text.txt") == NULL && errno == ENOTDIR);
    CHECK(opendir("absent") == NULL && errno == ENOENT);

    // Path folding
    CHECK(os_abspath("sub/./inner/../inner", abs, sizeof abs) != NULL);
    char expect[600];
    snprintf(expect, sizeof expect, "%s/sub/inner", cwd);
    CHECK(strcmp(abs, expect) == 0);
    CHECK(os_abspath("/a/b/../../..", abs, sizeof abs) && strcmp(abs, "/") == 0);

    // chdir and relative paths
    CHECK(chdir("sub/inner") == 0);
    CHECK(access("file.txt", F_OK) == 0);
    CHECK(chdir("../..") == 0);
    char again[512];
    CHECK(getcwd(again, sizeof again) && strcmp(again, cwd) == 0);
    CHECK(chdir("text.txt") == -1 && errno == ENOTDIR);

    // unlink / rmdir / remove semantics
    CHECK(unlink("sub") == -1 && errno == EISDIR);
    CHECK(rmdir("text.txt") == -1 && errno == ENOTDIR);
    CHECK(rmdir("sub/inner") == -1 && errno == ENOTEMPTY);

    // rename: same directory, across directories, replacing, and errors
    CHECK(rename("sub/inner/file.txt", "sub/inner/renamed.txt") == 0);
    CHECK(access("sub/inner/renamed.txt", F_OK) == 0 && access("sub/inner/file.txt", F_OK) == -1);
    CHECK(rename("sub/inner/renamed.txt", "moved.txt") == 0);
    CHECK(access("moved.txt", F_OK) == 0 && access("sub/inner/renamed.txt", F_OK) == -1);
    f = fopen("moved.txt", "r");
    CHECK(f && fgetc(f) == 'x');
    fclose(f);
    CHECK(rename("moved.txt", "log.txt") == 0);         // Replaces log.txt
    CHECK(stat("log.txt", &(struct stat){0}) == 0 && access("moved.txt", F_OK) == -1);
    // A directory to another parent: relinked by os_move (SYS_MOVE), contents and all
    CHECK(rename("sub/inner", "inner2") == 0 && dir_has(".", "inner2", DT_DIR) && !dir_has("sub", "inner", DT_DIR));
    CHECK(rename("inner2", "sub/inner") == 0 && dir_has("sub", "inner", DT_DIR));
    CHECK(os_move("sub", "sub/inner/x") == -1 && errno == EINVAL);
    CHECK(os_move("absent", "x") == -1 && errno == ENOENT);
    CHECK(rename("sub/inner", "sub/inner2") == 0 && dir_has("sub", "inner2", DT_DIR));
    CHECK(rename("sub", "sub/inner2/x") == -1 && errno == EINVAL);
    CHECK(rename("log.txt", "sub") == -1 && errno == EISDIR);
    CHECK(rename("absent", "x") == -1 && errno == ENOENT);

    CHECK(rmdir("sub/inner2") == 0 && rmdir("sub") == 0);
    CHECK(access("sub", F_OK) == -1);
}

//
// Home directory (the shim's home is the directory the test started in)
//

static void test_home(void) {
    char home[512], sys[512], expect[600];
    CHECK(os_gethome(home, sizeof home) != NULL && home[0] == '/');
    CHECK(os_gethome(home, 2) == NULL && errno == ERANGE);
    CHECK(os_gethome(home, 0) == NULL && errno == EINVAL);
    os_gethome(home, sizeof home);
    CHECK(getenv("HOME") != NULL && strcmp(getenv("HOME"), home) == 0);

    snprintf(expect, sizeof expect, "%s/sys/images", home);
    CHECK(os_syspath("images", sys, sizeof sys) && strcmp(sys, expect) == 0);
    CHECK(os_syspath("/images", sys, sizeof sys) && strcmp(sys, expect) == 0);
    snprintf(expect, sizeof expect, "%s/sys", home);
    CHECK(os_syspath(NULL, sys, sizeof sys) && strcmp(sys, expect) == 0);
    CHECK(os_syspath("", sys, sizeof sys) && strcmp(sys, expect) == 0);
    CHECK(os_syspath("images", sys, 4) == NULL && errno == ENAMETOOLONG);

    char* owned = os_gethome(NULL, 0);
    CHECK(owned && strcmp(owned, home) == 0);
    free(owned);
}

//
// Clock
//

static void test_time(void) {
    struct timespec a, b, res;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0 && a.tv_nsec >= 0 && a.tv_nsec < 1000000000L);
    CHECK(clock_gettime(CLOCK_BOOTTIME, &b) == 0);
    CHECK(clock_gettime(2, &a) == -1 && errno == EINVAL);
    CHECK(clock_gettime(CLOCK_MONOTONIC, NULL) == -1 && errno == EFAULT);
    CHECK(clock_getres(CLOCK_MONOTONIC, &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 1000000L);

    uint64_t t0 = os_time_ms();
    CHECK(t0 > 0);
    struct timespec nap = { 0, 25 * 1000000L }, rem = { 9, 9 };
    CHECK(nanosleep(&nap, &rem) == 0 && rem.tv_sec == 0 && rem.tv_nsec == 0);
    uint64_t t1 = os_time_ms();
    CHECK(t1 >= t0 + 25 && t1 < t0 + 2000);
    CHECK(clock() >= (clock_t)t1);
    struct timespec bad = { 0, 1000000000L };
    CHECK(nanosleep(&bad, NULL) == -1 && errno == EINVAL);

    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t ms = (uint64_t)a.tv_sec * 1000u + (uint64_t)(a.tv_nsec / 1000000L);
    CHECK(ms >= t1 && ms <= os_time_ms());

    // The wall clock (the shim's is the host's): after 2020, and what
    // time(), gettimeofday() and CLOCK_REALTIME say
    uint64_t wall = 0;
    CHECK(os_clock_ms(&wall) == 0 && wall > 1577836800000ull);
    uint64_t wall_before = wall;
    time_t now = time(NULL);
    CHECK(now >= (time_t)(wall / 1000u) && now < (time_t)(wall / 1000u) + 5);
    struct timespec rt;
    CHECK(clock_gettime(CLOCK_REALTIME, &rt) == 0 && rt.tv_sec >= (time_t)(wall / 1000u));
    struct timeval tv;
    CHECK(gettimeofday(&tv, NULL) == 0 && tv.tv_sec >= (time_t)(wall / 1000u) && tv.tv_usec < 1000000);
    struct tm tm;
    CHECK(localtime_r(&now, &tm) != NULL && tm.tm_year + 1900 >= 2020);

    // Setting it (the shim keeps an offset): 2030-06-15 12:00:00
    uint64_t want = 1907755200000ull;
    CHECK(os_clock_set(want) == 0);
    CHECK(os_clock_ms(&wall) == 0 && wall >= want && wall < want + 5000);
    now = time(NULL);
    CHECK(gmtime_r(&now, &tm) != NULL && tm.tm_year == 130 && tm.tm_mon == 5 && tm.tm_mday == 15 && tm.tm_hour == 12);
    CHECK(os_clock_set(915148800000ull) == -1 && errno == EINVAL);      // 1999
    struct timespec back = { (time_t)(wall_before / 1000u), 0 };
    CHECK(clock_settime(CLOCK_REALTIME, &back) == 0);
    CHECK(clock_settime(CLOCK_MONOTONIC, &back) == -1 && errno == EPERM);
    CHECK(os_clock_ms(&wall) == 0 && wall / 1000u >= wall_before / 1000u && wall / 1000u < wall_before / 1000u + 5);
}

//
// Security clearance (the shim starts the process with every bit)
//

static void test_security(void) {
    char text[96];
    sec_mask_t m, prev;

    CHECK(sec_get() == SEC_ALL && sec_is_admin() && sec_has(SEC_FILE | SEC_CONSOLE));
    CHECK(sec_get_process(0, &m) == 0 && m == SEC_ALL);
    CHECK(sec_get_process(getpid(), &m) == 0 && m == SEC_ALL);
    CHECK(sec_get_process(12345, &m) == -1 && errno == ESRCH);
    CHECK(sec_set_process(12345, SEC_ALL, NULL) == -1 && errno == ESRCH);
    CHECK(sec_set(0x40000) == -1 && errno == EINVAL && sec_get() == SEC_ALL);
    CHECK(sec_grant(0, 0x40000) == -1 && errno == EINVAL);

    // Names
    CHECK(sec_format(SEC_FILE_READ | SEC_CONSOLE, text, sizeof text) == 17 &&
          strcmp(text, "file_read|console") == 0);
    CHECK(sec_format(0, text, sizeof text) == 4 && strcmp(text, "none") == 0);
    CHECK(sec_format(SEC_ADMIN | 0x8000, text, sizeof text) > 0 && strcmp(text, "administration|0x8000") == 0);
    CHECK(sec_format(SEC_ALL, text, 6) == 135 && strcmp(text, "file_") == 0);      // Truncated like snprintf
    CHECK(sec_bit_name(SEC_GFX_SCREEN) && strcmp(sec_bit_name(SEC_GFX_SCREEN), "fullscreen") == 0);
    CHECK(sec_parse("process communication|fullscreen, exec", &m) == 0 && m == (SEC_IPC | SEC_GFX_SCREEN | SEC_EXEC));
    CHECK(sec_bit_name(SEC_FILE) == NULL);
    CHECK(sec_parse("file_read | GFX,admin", &m) == 0 && m == (SEC_FILE_READ | SEC_GFX | SEC_ADMIN));
    CHECK(sec_parse("file console 0x10", &m) == 0 && m == (SEC_FILE | SEC_CONSOLE | SEC_GFX_SCREEN));
    CHECK(sec_parse("all", &m) == 0 && m == SEC_ALL);
    CHECK(sec_parse("", &m) == 0 && m == 0);
    CHECK(sec_parse("file_read|bogus", &m) == -1 && errno == EINVAL);

    // Files written while allowed, then checked call by call as bits go away
    FILE* f = fopen("sec.txt", "w");
    CHECK(f && fputs("secret", f) >= 0 && fclose(f) == 0);
    int fd = open("sec.txt", O_RDWR);
    CHECK(fd >= 3);

    CHECK(sec_set_process(0, SEC_ALL & ~SEC_FILE_WRITE, &prev) == 0 && prev == SEC_ALL);
    CHECK(!sec_has(SEC_FILE_WRITE) && sec_has(SEC_FILE_READ));
    CHECK(fopen("sec.txt", "w") == NULL && errno == EPERM);
    CHECK(fopen("new.txt", "r+") == NULL && errno == EPERM);
    CHECK(open("new.txt", O_RDONLY | O_CREAT) == -1 && errno == EPERM);     // Creating is writing
    CHECK(write(fd, "x", 1) == -1 && errno == EPERM);                       // Already open: still refused
    CHECK(mkdir("secdir", 0755) == -1 && errno == EPERM);
    CHECK(remove("sec.txt") == -1 && errno == EPERM);
    CHECK(truncate("sec.txt", 0) == -1 && errno == EPERM);
    CHECK(os_copy("sec.txt", "copy.txt") == -1 && errno == EPERM);
    char b[8] = {0};
    CHECK(read(fd, b, 6) == 6 && memcmp(b, "secret", 6) == 0);              // Reading is still fine
    f = fopen("sec.txt", "r");
    CHECK(f && fgets(b, sizeof b, f) && strcmp(b, "secret") == 0);
    if (f) fclose(f);

    CHECK(sec_revoke(0, SEC_FILE_READ) == 0 && !sec_has(SEC_FILE_READ));
    struct stat st;
    CHECK(lseek(fd, 0, SEEK_SET) == 0);                                     // Always allowed
    CHECK(read(fd, b, 6) == -1 && errno == EPERM);
    CHECK(stat("sec.txt", &st) == -1 && errno == EPERM);
    CHECK(opendir(".") == NULL && errno == EPERM);
    CHECK(chdir(".") == -1 && errno == EPERM);
    char cwd[512];
    CHECK(getcwd(cwd, sizeof cwd) != NULL);                                 // Always allowed
    CHECK(close(fd) == 0);                                                  // Letting go always works

    CHECK(sec_grant(0, SEC_FILE) == 0 && sec_get() == SEC_ALL);
    CHECK(remove("sec.txt") == 0);

    // Console
    CHECK(sec_revoke(0, SEC_CONSOLE) == 0);
    CHECK(os_print("hidden\n", 7) == -1 && errno == EPERM);
    CHECK(write(1, "hidden\n", 7) == -1 && errno == EPERM);
    CHECK(sec_grant(0, SEC_CONSOLE) == 0 && os_print("", 0) == 0);

    // Admin dropping everything else, then itself: no way back
    CHECK(sec_set(SEC_CONSOLE | SEC_ADMIN) == 0 && sec_get() == (SEC_CONSOLE | SEC_ADMIN));
    CHECK(fopen("x.txt", "w") == NULL && errno == EPERM);
    CHECK(sec_set(SEC_CONSOLE | SEC_ADMIN | SEC_FILE_READ) == 0);
    CHECK(sec_drop(SEC_ADMIN) == 0 && !sec_is_admin());
    CHECK(sec_drop(SEC_FILE_READ) == 0 && sec_get() == SEC_CONSOLE);        // Dropping without admin
    CHECK(sec_grant(0, SEC_FILE_READ) == -1 && errno == EPERM);             // ... for good
    CHECK(os_clock_set(1907755200000ull) == -1 && errno == EPERM);     // Setting the clock needs admin
    CHECK(sec_set(SEC_ALL) == -1 && errno == EPERM);
    CHECK(sec_grant(0, SEC_FILE_READ) == -1 && errno == EPERM);
    CHECK(sec_get_process(12345, &m) == -1 && errno == EPERM);              // Others: admin only
    CHECK(sec_get_process(0, &m) == 0 && m == SEC_CONSOLE);                 // Ourselves: always
    CHECK(printf("%s", "") == 0);                                           // Console still works
    CHECK(sec_set(SEC_CONSOLE) == 0 && sec_get() == SEC_CONSOLE);           // Our own mask or less: allowed
}

//
// Exit path: atexit order and stdout flushed by exit()
//

static void at_exit_2(void) { printf("atexit: second registered, runs first\n"); }
static void at_exit_1(void) {
    printf("atexit: first registered, runs last\n");
    printf("unterminated line flushed by exit()");
}

//
// filetype_open: a script is run only when it is marked executable (the
// shim can't make a file executable, so only the refusal is checked)
//

static void test_filetype_open(void) {
    filetype_info_t info;
    memset(&info, 0, sizeof(info));
    strcpy(info.type, "Script");
    strcpy(info.open, "console --script");

    int fd = open("noexec.sh", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "echo hi\n", 8) == 8);
    close(fd);
    errno = 0;
    CHECK(filetype_open(&info, "noexec.sh", ".", sec_get()) < 0 && errno == EACCES);

    unlink("noexec.sh");

    // An image opens in paint, a viewer: no execute attribute needed
    static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    filetype_info_t image;
    filetype_lookup_header("photo.png", png, sizeof(png), &image);
    CHECK(strcmp(image.open, "paint") == 0 && image.viewer);
    fd = open("noexec.png", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, png, sizeof(png)) == (ssize_t)sizeof(png));
    close(fd);
    errno = 0;
    pid_t pid = filetype_open(&image, "noexec.png", ".", sec_get());
    CHECK(pid >= 0 || errno != EACCES);     // Paint may not be there: not refused, though
    unlink("noexec.png");
}

// A user library (sys/lib/stb, no extension) is known by its "ulib" header
// and gets the driver icon, as a "kdrv" driver does
static void test_filetype_library(void) {
    static const uint8_t ulib[8] = { 'u', 'l', 'i', 'b', 1, 0, 64, 0 };
    static const uint8_t kdrv[8] = { 'k', 'd', 'r', 'v', 1, 0, 64, 0 };
    filetype_info_t t;
    filetype_lookup_header("stb", ulib, sizeof(ulib), &t);
    CHECK(strcmp(t.type, "Library") == 0 && strcmp(t.icon, "icon_driver") == 0);
    filetype_lookup_header("hello", kdrv, sizeof(kdrv), &t);
    CHECK(strcmp(t.type, "Driver") == 0 && strcmp(t.icon, "icon_driver") == 0);
}

int main(int argc, char** argv) {
    CHECK(argc == 0 && argv[0] == NULL);
    CHECK(isatty(1) && !isatty(5));

    test_malloc();
    test_misc();
    test_files();
    test_dirs();
    test_home();
    test_time();
    test_filetype_open();
    test_filetype_library();
    test_security();       // Last: it ends by giving up everything but the console

    assert(1 + 1 == 2);
    fprintf(stderr, "stderr: unbuffered %s\n", "output");

    printf("%d checks, %d failures\n", checks, failures);
    atexit(at_exit_1);
    atexit(at_exit_2);
    return failures ? 1 : 0;
}


