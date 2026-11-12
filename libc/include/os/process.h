#ifndef _OS_PROCESS_H
#define _OS_PROCESS_H

/*
 * Starting other programs. Needs SEC_EXEC (os/security.h), which the
 * default clearance includes.
 *
 *     char* args[] = { "hello", "--loud", "world", NULL };
 *     pid_t pid = spawn("/mnt/ata0p1/bin/hello", args);    // pid, or -1 with errno
 *
 *     spawnl("hello", "hello", "--loud", NULL);           // relative to the cwd
 *
 * The new program receives argv in main(argc, argv) exactly as given. By
 * convention argv[0] is its name; a NULL or empty argv gives it
 * argv = { the resolved path }. The file must be an ELF executable marked
 * executable, and this program's file clearance must allow reading it.
 *
 * spawn() returns as soon as the program is loaded; both then run side by
 * side. The child starts in this program's working directory with this
 * program's clearance and priority, unless spawn_ex() says otherwise.
 *
 * Errors: -1 with errno
 *   ENOENT   no such file              EISDIR   it is a directory
 *   EACCES   not marked executable, or the file needs clearance we lack
 *   ENOEXEC  not an ELF this kernel can run
 *   E2BIG    more than SPAWN_ARG_MAX strings or SPAWN_ARG_BYTES bytes of them
 *   EAGAIN   the process table is full ENOMEM   out of memory
 *   EPERM    no SEC_EXEC, or spawn_ex() asked for bits this process lacks
 */

#include <sys/types.h>
#include <os/syscall.h>
#include <os/security.h>

#define SPAWN_ARG_MAX       SYS_ARG_MAX         // Strings in argv
#define SPAWN_ARG_BYTES     SYS_ARG_BYTES       // Bytes of them, terminators included

#define SPAWN_INHERIT       SYS_SPAWN_INHERIT   // spawn_ex security: this process's own mask
#define SPAWN_CWD_PROGRAM   ""                  // spawn_ex cwd: the executable's directory
#define SPAWN_ELEVATE       SYS_SPAWN_ELEVATE   // spawn_ex security flag: with what its file needs (below)

// Start `path` with `argv` (NULL-terminated; may be NULL). Process id, or -1.
pid_t spawn(const char* path, char* const argv[]);

// The same with the arguments listed in the call, ending with NULL:
// spawnl(path, arg0, arg1, ..., NULL)
pid_t spawnl(const char* path, const char* arg0, ...);

// spawn() with a choice of working directory and clearance.
//   cwd       NULL: this program's; SPAWN_CWD_PROGRAM: the executable's own
//             directory; anything else a path (relative to ours). One that is
//             not a directory starts the child in "/".
//   security  SPAWN_INHERIT, or a mask holding only bits this process has
//             (a child can be given less clearance, never more)
//
// Starting a program with what its file needs (its "admin", "network" or
// "system" policy, or any bit above a user program's): add SPAWN_ELEVATE
// to the mask. The child gets the file's security mask on top of it (and
// is elevated when that includes SEC_ADMIN). For bits we don't hold
// ourselves (SEC_ELEVATE is needed then), the administrator prompt comes
// up first, naming the program, us as the one starting it and those bits,
// and the call blocks until the user answers; we never hold them
// ourselves. The extra errors:
//   ECANCELED  the user chose Cancel (nothing to tell them: they know)
//   ETIMEDOUT, EAGAIN, EBUSY  as for sec_elevate() (os/security.h)
pid_t spawn_ex(const char* path, char* const argv[], const char* cwd, sec_mask_t security);

// spawn_ex() on a terminal: the child's stdin, stdout and stderr are the
// pipe end `tty` (from ipc_pipe()). The child keeps its own reference, so
// close `tty` right after; read and write the other end. Each read(0) in
// the child gets the next message sent (or what fits of it); everything it
// writes arrives as messages. Needs SEC_EXEC and SEC_IPC.
pid_t spawn_tty(const char* path, char* const argv[], const char* cwd, sec_mask_t security, int tty);

// The size the child's ioctl(TIOCGWINSZ) reports, set from either end of
// the pipe. A new pipe says 80 x 25.
int tty_resize(int pipe, unsigned columns, unsigned rows);

/*
 * Program kinds: windowed or console
 *
 * Like the subsystem field of a Windows executable, a program carries its
 * kind in its file, so a launcher can tell before starting it whether it
 * needs a console window. Any program that uses the window manager
 * (os/wm.h, os/wm_app.h) is marked windowed by the libc's wm code: nothing
 * to do in the program. Everything else is a console program. A program
 * can say so itself, once, at file scope in any of its .c files:
 *
 *     PROGRAM_KIND_CONSOLE;       // uses the wm, but wants a console too
 *     PROGRAM_KIND_WINDOWED;      // no wm, and no console either
 *
 * When both marks are in a file, console wins. The mark is a section the
 * loader never maps (.os.kind in user.ld), read from the file by
 * program_kind().
 */

#define PROGRAM_CONSOLE     0
#define PROGRAM_WINDOWED    1

#define __PROGRAM_KIND(kind) \
    __asm__(".pushsection .os.kind,\"\",@progbits\n.asciz \"" kind "\"\n.popsection")
#define PROGRAM_KIND_CONSOLE    __PROGRAM_KIND("console")
#define PROGRAM_KIND_WINDOWED   __PROGRAM_KIND("windowed")

// PROGRAM_CONSOLE or PROGRAM_WINDOWED for the ELF file at `path`; -1 with
// errno if it can't be read (ENOEXEC: not an ELF file). A program we have
// no clearance to read is still answered, from what the kernel read
// (os_finfo, SYS_FINFO_CONSOLE / _WINDOWED).
int program_kind(const char* path);

// 1 when the program at `path` needs bits a child with `security` (the
// mask a launcher would give it) lacks, and we may ask for them
// (SEC_ELEVATE) or hold them all. Start it with SPAWN_ELEVATE then. 0 when
// it isn't so, -1 with errno when the file can't be looked at.
int program_needs_elevation(const char* path, sec_mask_t security);

// Start a program the way the desktop does: a windowed program as
// spawn_ex() would, a console program on a terminal in a new console
// window (<home>/bin/console --run path args...), or as spawn_ex() would
// when there is no console program to host it. Same arguments, result and
// errors as spawn_ex(); the pid is the console window's for a console
// program.
//
// With SPAWN_ELEVATE, a console program's window is started without it:
// the console asks for the program itself when it starts it, so the
// prompt names the program and not the console.
pid_t spawn_program(const char* path, char* const argv[], const char* cwd, sec_mask_t security);

#endif
