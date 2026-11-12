# TinyCC on the kernel

`build.sh` builds TinyCC (tested with tinycc mob, 0.9.28rc) as a program for
the kernel, linked against this libc, plus the tree it reads at run time.

```sh
cmake -S . -B build && cmake --build build          # the libc, from libc/
git clone https://github.com/TinyCC/tinycc.git /tmp/tinycc
ports/tcc/build.sh /tmp/tinycc out
```

Copy `out/tcc` to the disk wherever programs live, and `out/sys/tcc/` to
`<home>/sys/tcc/` (`os_syspath("tcc")`). Then, on the kernel:

```sh
tcc hello.c -o hello         # static ELF at 0x40000000, what the loader wants
tcc -run hello.c             # compile in memory and run
tcc -B/other/dir ...         # tcc files somewhere else
```

`config.h` holds the settings (paths, `-static`, the load address).

## What was checked

Off the kernel, with the kernel's syscalls emulated on Linux (`int 0x81`
trapped and forwarded) and a 64 KB stack like `PROCESS_STACK_PAGES`:

- tcc compiles and links hello-world style programs with no extra flags,
  and `tcc -run` works.
- tcc compiles its own source (`tcc.c`, one unit). The tcc built that way
  compiles it again to a byte-identical object.
- Compile errors are reported and tcc exits cleanly (setjmp/longjmp).
- `kernel/resources/classes/elf.c`'s `elf_load`, built on the host with
  the file and page-table calls stubbed, accepts `tcc` and the programs it
  writes.

Not yet run on the kernel itself.

## Limits

- `time()` counts from boot (no real-time clock), so `__DATE__` reads
  `Jan  1 1970`.
- `math.h` has no transcendental functions (sin, exp, log, pow, ...).
- tcc's `-m64`/cross switching (`execvp`) and backtraces are off.
