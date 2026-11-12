#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF             (-1)
#define BUFSIZ          4096
#define FILENAME_MAX    512
#define FOPEN_MAX       32
#define L_tmpnam        32

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

#define _IOFBF          0
#define _IOLBF          1
#define _IONBF          2

typedef struct __FILE FILE;
typedef long fpos_t;

/*
 * stdout is line buffered, stderr unbuffered; both go to the console, or to
 * the terminal the program was started on (spawn_tty()). stdin reads from
 * that terminal; without one, reads fail (ferror set, errno EBADF).
 */
extern FILE* const stdin;
extern FILE* const stdout;
extern FILE* const stderr;

/* Files */
FILE*  fopen(const char* restrict path, const char* restrict mode);
FILE*  fdopen(int fd, const char* mode);
FILE*  freopen(const char* restrict path, const char* restrict mode, FILE* restrict stream);
int    fclose(FILE* stream);
int    fflush(FILE* stream);
int    setvbuf(FILE* restrict stream, char* restrict buf, int mode, size_t size);
void   setbuf(FILE* restrict stream, char* restrict buf);
int    fileno(FILE* stream);

/* Block I/O */
size_t fread(void* restrict ptr, size_t size, size_t count, FILE* restrict stream);
size_t fwrite(const void* restrict ptr, size_t size, size_t count, FILE* restrict stream);

/* Character I/O */
int    fgetc(FILE* stream);
int    getc(FILE* stream);
int    getchar(void);
int    ungetc(int c, FILE* stream);
char*  fgets(char* restrict s, int size, FILE* restrict stream);
ssize_t getline(char** restrict line, size_t* restrict cap, FILE* restrict stream);
ssize_t getdelim(char** restrict line, size_t* restrict cap, int delim, FILE* restrict stream);
int    fputc(int c, FILE* stream);
int    putc(int c, FILE* stream);
int    putchar(int c);
int    fputs(const char* restrict s, FILE* restrict stream);
int    puts(const char* s);

/* Position */
int    fseek(FILE* stream, long offset, int whence);
long   ftell(FILE* stream);
void   rewind(FILE* stream);
int    fgetpos(FILE* restrict stream, fpos_t* restrict pos);
int    fsetpos(FILE* stream, const fpos_t* pos);

/* Status */
int    feof(FILE* stream);
int    ferror(FILE* stream);
void   clearerr(FILE* stream);
void   perror(const char* s);

/* Formatted output. Supports %d %i %u %o %x %X %c %s %p %n %% %f %F %e %E
   %g %G, flags "-+ #0", width/precision (incl. *), lengths hh h l ll j z t L. */
int    printf(const char* restrict fmt, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE* restrict stream, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));
int    dprintf(int fd, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));
int    sprintf(char* restrict buf, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char* restrict buf, size_t size, const char* restrict fmt, ...) __attribute__((format(printf, 3, 4)));
int    asprintf(char** restrict out, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));
int    vprintf(const char* restrict fmt, va_list ap);
int    vfprintf(FILE* restrict stream, const char* restrict fmt, va_list ap);
int    vdprintf(int fd, const char* restrict fmt, va_list ap);
int    vsprintf(char* restrict buf, const char* restrict fmt, va_list ap);
int    vsnprintf(char* restrict buf, size_t size, const char* restrict fmt, va_list ap);
int    vasprintf(char** restrict out, const char* restrict fmt, va_list ap);

/* Files by name */
int    remove(const char* path);
int    rename(const char* old_path, const char* new_path);

#endif

