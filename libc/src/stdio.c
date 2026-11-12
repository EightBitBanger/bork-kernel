/*
 * Buffered streams on top of read/write/lseek.
 *
 * Each FILE has one buffer used for either direction: it holds read-ahead
 * data (rpos..rend) or pending output (0..wlen), never both. Switching
 * direction flushes output or gives read-ahead back with lseek, so mixing
 * reads, writes and seeks behaves as in any C library.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/os.h>
#include "internal.h"

#define F_READ      0x0001      // Opened for reading
#define F_WRITE     0x0002      // Opened for writing
#define F_APPEND    0x0004
#define F_EOF       0x0010
#define F_ERR       0x0020
#define F_LBF       0x0040      // Line buffered
#define F_NBF       0x0080      // Unbuffered
#define F_OWNBUF    0x0100      // buf came from malloc
#define F_STATIC    0x0200      // stdin/stdout/stderr: never freed

struct __FILE {
    int            fd;
    unsigned       flags;
    unsigned char* buf;
    size_t         size;        // Buffer capacity
    size_t         rpos, rend;  // Read-ahead window
    size_t         wlen;        // Pending output bytes
    int            ungot;       // ungetc() byte, or EOF
    unsigned char  tiny;        // 1-byte buffer for unbuffered streams
    struct __FILE* next;        // All open streams (fflush(NULL), exit)
};

static FILE f_stdin  = { 0, F_READ  | F_STATIC,          NULL, 0, 0, 0, 0, EOF, 0, NULL };
static FILE f_stdout = { 1, F_WRITE | F_STATIC | F_LBF,  NULL, 0, 0, 0, 0, EOF, 0, NULL };
static FILE f_stderr = { 2, F_WRITE | F_STATIC | F_NBF,  NULL, 0, 0, 0, 0, EOF, 0, NULL };

FILE* const stdin  = &f_stdin;
FILE* const stdout = &f_stdout;
FILE* const stderr = &f_stderr;

static FILE* open_list = NULL;   // Streams from fopen/fdopen

//
// Buffer management
//

// Give the stream its buffer on first use
static void ensure_buffer(FILE* f) {
    if (f->buf != NULL)
        return;
    if (!(f->flags & F_NBF)) {
        f->buf = (unsigned char*)malloc(BUFSIZ);
        if (f->buf != NULL) {
            f->size = BUFSIZ;
            f->flags |= F_OWNBUF;
            return;
        }
        f->flags |= F_NBF;      // No memory: fall back to unbuffered
    }
    f->buf = &f->tiny;
    f->size = 1;
}

// Write all of [p, p+n) to the descriptor. False (F_ERR set) on failure.
static bool write_all(FILE* f, const unsigned char* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(f->fd, p, n);
        if (w <= 0) {
            f->flags |= F_ERR;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static int flush_write(FILE* f) {
    if (f->wlen == 0)
        return 0;
    size_t n = f->wlen;
    f->wlen = 0;
    return write_all(f, f->buf, n) ? 0 : EOF;
}

// Drop read-ahead, moving the descriptor back to the logical position
static int drop_read(FILE* f) {
    size_t ahead = (f->rend - f->rpos) + (f->ungot != EOF ? 1 : 0);
    f->rpos = f->rend = 0;
    f->ungot = EOF;
    if (ahead > 0 && lseek(f->fd, -(off_t)ahead, SEEK_CUR) < 0)
        return EOF;
    return 0;
}

// Refill the read buffer. Returns false at end of file or on error.
static bool fill(FILE* f) {
    if (!(f->flags & F_READ)) {
        f->flags |= F_ERR;
        errno = EBADF;
        return false;
    }
    if (f->wlen > 0 && flush_write(f) == EOF)
        return false;

    // Reading from an input stream flushes line-buffered output (C 7.21.3)
    if (f == stdin && stdout->wlen > 0)
        flush_write(stdout);

    ensure_buffer(f);
    ssize_t r = read(f->fd, f->buf, f->size);
    if (r <= 0) {
        f->flags |= (r == 0) ? F_EOF : F_ERR;
        return false;
    }
    f->rpos = 0;
    f->rend = (size_t)r;
    return true;
}

//
// Opening and closing
//

// "r", "w+", "ab", "rx"... -> open() flags and stream flags
static bool parse_mode(const char* mode, int* oflags, unsigned* fflags) {
    int acc, extra = 0;
    unsigned ff;

    switch (*mode) {
        case 'r': acc = O_RDONLY; ff = F_READ;               break;
        case 'w': acc = O_WRONLY; ff = F_WRITE; extra = O_CREAT | O_TRUNC;  break;
        case 'a': acc = O_WRONLY; ff = F_WRITE | F_APPEND; extra = O_CREAT | O_APPEND; break;
        default:  return false;
    }
    for (const char* m = mode + 1; *m; m++) {
        if (*m == '+')      { acc = O_RDWR; ff |= F_READ | F_WRITE; }
        else if (*m == 'x') { extra |= O_EXCL; }
        // 'b' and anything else: ignored, as POSIX allows
    }
    *oflags = acc | extra;
    *fflags = ff;
    return true;
}

static FILE* new_stream(int fd, unsigned flags) {
    FILE* f = (FILE*)calloc(1, sizeof(FILE));
    if (f == NULL)
        return NULL;
    f->fd = fd;
    f->flags = flags;
    f->ungot = EOF;
    f->next = open_list;
    open_list = f;
    return f;
}

FILE* fopen(const char* restrict path, const char* restrict mode) {
    int oflags;
    unsigned fflags;
    if (!parse_mode(mode, &oflags, &fflags)) {
        errno = EINVAL;
        return NULL;
    }

    int fd = open(path, oflags, 0666);
    if (fd < 0)
        return NULL;

    FILE* f = new_stream(fd, fflags);
    if (f == NULL)
        close(fd);
    return f;
}

FILE* fdopen(int fd, const char* mode) {
    int oflags;
    unsigned fflags;
    if (!parse_mode(mode, &oflags, &fflags)) {
        errno = EINVAL;
        return NULL;
    }
    if (fd >= 0 && fd < SYS_FD_FIRST)
        fflags |= F_LBF;    // Console
    return new_stream(fd, fflags);
}

static void unlink_stream(FILE* f) {
    for (FILE** link = &open_list; *link; link = &(*link)->next) {
        if (*link == f) {
            *link = f->next;
            return;
        }
    }
}

int fclose(FILE* f) {
    int r = fflush(f);

    if (f->flags & F_OWNBUF)
        free(f->buf);
    f->buf = NULL;
    f->size = 0;

    if (f->flags & F_STATIC) {
        // Standard streams stay usable objects; the descriptor is the console
        f->flags |= F_EOF;
        return r;
    }

    if (close(f->fd) < 0)
        r = EOF;
    unlink_stream(f);
    free(f);
    return r;
}

FILE* freopen(const char* restrict path, const char* restrict mode, FILE* restrict f) {
    int oflags;
    unsigned fflags;

    fflush(f);
    if (path == NULL || !parse_mode(mode, &oflags, &fflags)) {
        errno = EINVAL;     // Changing the mode of an open stream: not supported
        return NULL;
    }

    int fd = open(path, oflags, 0666);
    if (fd < 0)
        return NULL;

    if (!(f->flags & F_STATIC) || f->fd >= SYS_FD_FIRST)
        close(f->fd);
    if (f->flags & F_OWNBUF)
        free(f->buf);

    f->fd = fd;
    f->flags = fflags | (f->flags & F_STATIC);
    f->buf = NULL;
    f->size = f->rpos = f->rend = f->wlen = 0;
    f->ungot = EOF;
    return f;
}

int setvbuf(FILE* restrict f, char* restrict buf, int mode, size_t size) {
    // Only before the first I/O, as C requires
    if (f->rend || f->wlen)
        return -1;

    if (f->flags & F_OWNBUF)
        free(f->buf);
    f->flags &= ~(F_LBF | F_NBF | F_OWNBUF);
    f->buf = NULL;
    f->size = 0;

    switch (mode) {
        case _IONBF:
            f->flags |= F_NBF;
            return 0;
        case _IOLBF:
            f->flags |= F_LBF;
            break;
        case _IOFBF:
            break;
        default:
            return -1;
    }
    if (buf != NULL && size > 0) {
        f->buf = (unsigned char*)buf;
        f->size = size;
    }
    return 0;
}

void setbuf(FILE* restrict f, char* restrict buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int fileno(FILE* f) {
    return f->fd;
}

int fflush(FILE* f) {
    if (f == NULL) {
        int r = 0;
        if (flush_write(stdout) == EOF) r = EOF;
        if (flush_write(stderr) == EOF) r = EOF;
        for (FILE* p = open_list; p; p = p->next) {
            if (flush_write(p) == EOF) r = EOF;
        }
        return r;
    }
    if (f->wlen > 0)
        return flush_write(f);
    if (f->rend > f->rpos || f->ungot != EOF)
        return (f->fd >= SYS_FD_FIRST) ? drop_read(f) : 0;
    return 0;
}

void __stdio_exit(void) {
    fflush(NULL);
}

//
// Writing
//

size_t fwrite(const void* restrict ptr, size_t size, size_t count, FILE* restrict f) {
    size_t total;
    if (size == 0 || count == 0)
        return 0;
    if (__builtin_mul_overflow(size, count, &total)) {
        errno = EOVERFLOW;
        f->flags |= F_ERR;
        return 0;
    }
    if (!(f->flags & F_WRITE)) {
        errno = EBADF;
        f->flags |= F_ERR;
        return 0;
    }

    // Leaving read mode: give the read-ahead back first
    if ((f->rend > f->rpos || f->ungot != EOF) && drop_read(f) == EOF) {
        f->flags |= F_ERR;
        return 0;
    }
    f->rpos = f->rend = 0;

    const unsigned char* p = (const unsigned char*)ptr;
    size_t left = total;
    ensure_buffer(f);

    if (f->flags & F_NBF) {
        if (!write_all(f, p, left))
            return 0;
        return count;
    }

    // Larger than the buffer: write straight through after any pending data
    if (left >= f->size) {
        if (flush_write(f) == EOF || !write_all(f, p, left))
            return 0;
        return count;
    }

    while (left > 0) {
        size_t room = f->size - f->wlen;
        size_t n = (left < room) ? left : room;
        memcpy(f->buf + f->wlen, p, n);
        f->wlen += n;
        p += n;
        left -= n;
        if (f->wlen == f->size && flush_write(f) == EOF)
            return 0;   // How much of the buffer reached the file is unknown
    }

    if ((f->flags & F_LBF) && memchr(ptr, '\n', total) != NULL) {
        if (flush_write(f) == EOF)
            return 0;
    }
    return count;
}

int fputc(int c, FILE* f) {
    unsigned char ch = (unsigned char)c;

    // Fast path: room in a fully buffered stream already in write mode
    if (f->wlen > 0 && f->wlen < f->size && !(f->flags & (F_LBF | F_NBF))) {
        f->buf[f->wlen++] = ch;
        return ch;
    }
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}

int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char* restrict s, FILE* restrict f) {
    size_t n = strlen(s);
    if (n == 0)
        return 0;
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char* s) {
    if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF)
        return EOF;
    return 0;
}

//
// Reading
//

size_t fread(void* restrict ptr, size_t size, size_t count, FILE* restrict f) {
    size_t total;
    if (size == 0 || count == 0)
        return 0;
    if (__builtin_mul_overflow(size, count, &total)) {
        errno = EOVERFLOW;
        f->flags |= F_ERR;
        return 0;
    }

    unsigned char* p = (unsigned char*)ptr;
    size_t done = 0;

    if (f->ungot != EOF && total > 0) {
        p[done++] = (unsigned char)f->ungot;
        f->ungot = EOF;
    }

    while (done < total) {
        size_t avail = f->rend - f->rpos;
        if (avail > 0) {
            size_t n = (total - done < avail) ? total - done : avail;
            memcpy(p + done, f->buf + f->rpos, n);
            f->rpos += n;
            done += n;
            continue;
        }

        // Large remainder: read straight into the caller's buffer
        ensure_buffer(f);
        if (total - done >= f->size && (f->flags & F_READ) && f->wlen == 0) {
            ssize_t r = read(f->fd, p + done, total - done);
            if (r <= 0) {
                f->flags |= (r == 0) ? F_EOF : F_ERR;
                break;
            }
            done += (size_t)r;
            continue;
        }

        if (!fill(f))
            break;
    }
    return done / size;
}

int fgetc(FILE* f) {
    if (f->ungot != EOF) {
        int c = f->ungot;
        f->ungot = EOF;
        return c;
    }
    if (f->rpos < f->rend)
        return f->buf[f->rpos++];
    if (!fill(f))
        return EOF;
    return f->buf[f->rpos++];
}

int getc(FILE* f)  { return fgetc(f); }
int getchar(void)  { return fgetc(stdin); }

int ungetc(int c, FILE* f) {
    if (c == EOF || f->ungot != EOF)
        return EOF;     // One byte of pushback, as C guarantees
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

char* fgets(char* restrict s, int size, FILE* restrict f) {
    if (size <= 0)
        return NULL;

    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    s[i] = '\0';
    return s;
}

ssize_t getdelim(char** restrict line, size_t* restrict cap, int delim, FILE* restrict f) {
    if (line == NULL || cap == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*line == NULL)
        *cap = 0;

    size_t len = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) {
            if (len == 0)
                return -1;
            break;
        }
        if (len + 2 > *cap) {
            size_t ncap = *cap ? *cap * 2 : 128;
            char* p = (char*)realloc(*line, ncap);
            if (p == NULL) {
                f->flags |= F_ERR;
                return -1;
            }
            *line = p;
            *cap = ncap;
        }
        (*line)[len++] = (char)c;
        if (c == delim)
            break;
    }
    (*line)[len] = '\0';
    return (ssize_t)len;
}

ssize_t getline(char** restrict line, size_t* restrict cap, FILE* restrict f) {
    return getdelim(line, cap, '\n', f);
}

//
// Positioning and status
//

int fseek(FILE* f, long offset, int whence) {
    if (whence == SEEK_CUR) {
        // Relative to the logical position, not the descriptor's
        long pos = ftell(f);
        if (pos < 0)
            return -1;
        offset += pos;
        whence = SEEK_SET;
    }
    if (flush_write(f) == EOF)
        return -1;
    f->rpos = f->rend = 0;
    f->ungot = EOF;
    if (lseek(f->fd, offset, whence) < 0)
        return -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE* f) {
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    pos -= (off_t)(f->rend - f->rpos);
    if (f->ungot != EOF) pos--;
    pos += (off_t)f->wlen;
    return pos;
}

void rewind(FILE* f) {
    fseek(f, 0, SEEK_SET);
    f->flags &= ~F_ERR;
}

int fgetpos(FILE* restrict f, fpos_t* restrict pos) {
    long p = ftell(f);
    if (p < 0)
        return -1;
    *pos = p;
    return 0;
}

int fsetpos(FILE* f, const fpos_t* pos) {
    return fseek(f, *pos, SEEK_SET);
}

int  feof(FILE* f)     { return (f->flags & F_EOF) != 0; }
int  ferror(FILE* f)   { return (f->flags & F_ERR) != 0; }
void clearerr(FILE* f) { f->flags &= ~(F_EOF | F_ERR); }

void perror(const char* s) {
    const char* msg = strerror(errno);
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, msg);
    else
        fprintf(stderr, "%s\n", msg);
}

//
// Formatted output to streams and descriptors
//

// Output is gathered in a small local buffer and handed to fwrite() in
// pieces, so an unbuffered stream (stderr) gets one write per 256 bytes
// rather than one per format fragment
typedef struct {
    FILE*  f;
    char   buf[256];
    size_t len;
} filebuf_t;

static void emit_file(void* ctx, const char* s, size_t n) {
    filebuf_t* b = (filebuf_t*)ctx;
    if (n >= sizeof(b->buf)) {
        if (b->len) { fwrite(b->buf, 1, b->len, b->f); b->len = 0; }
        fwrite(s, 1, n, b->f);
        return;
    }
    if (b->len + n > sizeof(b->buf)) {
        fwrite(b->buf, 1, b->len, b->f);
        b->len = 0;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

int vfprintf(FILE* restrict f, const char* restrict fmt, va_list ap) {
    // Line-buffered: hold the whole call, then flush once, so one printf()
    // reaches the console in one piece rather than one write per line
    unsigned saved = f->flags & F_LBF;
    f->flags &= ~F_LBF;
    unsigned err_before = f->flags & F_ERR;

    filebuf_t b;
    b.f = f;
    b.len = 0;
    int r = __vformat(emit_file, &b, fmt, ap);
    if (b.len) fwrite(b.buf, 1, b.len, f);

    f->flags |= saved;
    if (saved && f->wlen > 0 && memchr(f->buf, '\n', f->wlen) != NULL)
        flush_write(f);

    if (!err_before && (f->flags & F_ERR))
        return -1;
    return r;
}

int fprintf(FILE* restrict f, const char* restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char* restrict fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char* restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

typedef struct {
    int    fd;
    char   buf[256];
    size_t len;
    bool   failed;
} fdbuf_t;

static void fdbuf_flush(fdbuf_t* b) {
    size_t off = 0;
    while (off < b->len && !b->failed) {
        ssize_t w = write(b->fd, b->buf + off, b->len - off);
        if (w <= 0) b->failed = true;
        else        off += (size_t)w;
    }
    b->len = 0;
}

static void emit_fd(void* ctx, const char* s, size_t n) {
    fdbuf_t* b = (fdbuf_t*)ctx;
    while (n > 0) {
        if (b->len == sizeof(b->buf))
            fdbuf_flush(b);
        size_t room = sizeof(b->buf) - b->len;
        size_t k = (n < room) ? n : room;
        memcpy(b->buf + b->len, s, k);
        b->len += k;
        s += k;
        n -= k;
    }
}

int vdprintf(int fd, const char* restrict fmt, va_list ap) {
    fdbuf_t b;
    b.fd = fd;
    b.len = 0;
    b.failed = false;
    int r = __vformat(emit_fd, &b, fmt, ap);
    fdbuf_flush(&b);
    return b.failed ? -1 : r;
}

int dprintf(int fd, const char* restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

//
// Files by name
//

int remove(const char* path) {
    // Files, and directories once empty (SYS_REMOVE refuses anything else)
    return __sys_ret(syscall1(SYS_REMOVE, (uint32_t)path));
}

// Split an absolute, normalized path into its directory and its last name
static void split_path(char* path, char** dir, char** name) {
    char* slash = strrchr(path, '/');
    *name = slash + 1;
    if (slash == path) {
        *dir = "/";
    } else {
        *slash = '\0';
        *dir = path;
    }
}

/*
 * Within one directory this is os_rename. Across directories on one volume
 * it is os_move, which relinks the entry without copying anything (files
 * and directories alike). Across volumes (EXDEV from os_move) a file is
 * copied and the original removed; a directory stays EXDEV. As in POSIX, an
 * existing target is replaced (a file by a file, an empty directory by a
 * directory), though not atomically.
 */
int rename(const char* old_path, const char* new_path) {
    char from[SYS_PATH_MAX], to[SYS_PATH_MAX];
    if (os_abspath(old_path, from, sizeof(from)) == NULL ||
        os_abspath(new_path, to, sizeof(to)) == NULL)
        return -1;

    struct stat src;
    if (stat(from, &src) < 0)
        return -1;
    if (strcmp(from, to) == 0)
        return 0;
    if (strcmp(from, "/") == 0 || strcmp(to, "/") == 0) {
        errno = EBUSY;
        return -1;
    }

    // A directory cannot move inside itself
    size_t from_len = strlen(from);
    if (S_ISDIR(src.st_mode) && strncmp(to, from, from_len) == 0 && to[from_len] == '/') {
        errno = EINVAL;
        return -1;
    }

    char from_split[SYS_PATH_MAX], to_split[SYS_PATH_MAX];
    char *from_dir, *from_name, *to_dir, *to_name;
    memcpy(from_split, from, sizeof(from));
    memcpy(to_split, to, sizeof(to));
    split_path(from_split, &from_dir, &from_name);
    split_path(to_split, &to_dir, &to_name);
    bool same_dir = strcmp(from_dir, to_dir) == 0;
    (void)from_name;

    struct stat dst;
    if (stat(to, &dst) == 0) {
        if (S_ISDIR(dst.st_mode) && !S_ISDIR(src.st_mode)) { errno = EISDIR;  return -1; }
        if (!S_ISDIR(dst.st_mode) && S_ISDIR(src.st_mode)) { errno = ENOTDIR; return -1; }
        if (remove(to) < 0)
            return -1;      // ENOTEMPTY for a full directory, EBUSY if open
    } else if (errno != ENOENT) {
        return -1;
    }

    if (same_dir)
        return os_rename(from, to_name);

    // Same volume: relink, nothing is copied
    if (os_move(from, to) == 0)
        return 0;
    if (errno != EXDEV || S_ISDIR(src.st_mode))
        return -1;

    if (os_copy(from, to) < 0)
        return -1;
    if (remove(from) < 0) {
        int saved = errno;
        remove(to);         // Don't leave two copies behind
        errno = saved;
        return -1;
    }
    return 0;
}

