#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

#define F_OK            0
#define X_OK            1
#define W_OK            2
#define R_OK            4

extern char** environ;

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);

int     unlink(const char* path);
int     rmdir(const char* path);
int     truncate(const char* path, off_t length);
int     access(const char* path, int mode);
int     chdir(const char* path);
char*   getcwd(char* buf, size_t size);
int     isatty(int fd);
void    sync(void);         // os_sync() with the error dropped

pid_t   getpid(void);

/* No process replacement: the kernel starts programs side by side
   (os/process.h spawn()). These fail with ENOSYS. */
int     execv(const char* path, char* const argv[]);
int     execvp(const char* file, char* const argv[]);
int     execve(const char* path, char* const argv[], char* const envp[]);
void    _exit(int status) __attribute__((noreturn));
void*   sbrk(intptr_t increment);
int     brk(void* addr);

unsigned int sleep(unsigned int seconds);
int     usleep(useconds_t usec);

#endif


