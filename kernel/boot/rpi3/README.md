# Raspberry Pi 3 Model B+ boot path

Target: **Raspberry Pi 3 Model B+** (BCM2837B0, 4x Cortex-A53, 1 GB), 64-bit
(aarch64), booted by the Pi firmware from an SD card. The same kernel runs on
a Pi 3B, 3A+ and Zero 2 W. A Pi 4 needs different addresses and a GIC
(`kernel/boot/rpi4/` has notes for that board; it is not built).

Status: a boot stub. It brings up the serial console, asks the firmware for
the board revision and memory size over the mailbox, checks the exception
vectors (`brk`, `svc`) and the generic timer, then echoes serial input.

## Build and run

```
cmake -S . -B build-rpi3 -DKERNEL_TARGET=rpi3
cmake --build build-rpi3                  # bin/kernel8.img, bin/rpi3-boot/
cmake --build build-rpi3 --target run     # QEMU raspi3b, serial on stdio
```

Any of these compilers works, found automatically
(`cmake/toolchain-aarch64.cmake`): `aarch64-none-elf-gcc` (Arm GNU
Toolchain), `aarch64-linux-gnu-gcc` (`apt install gcc-aarch64-linux-gnu`), or
clang with ld.lld. QEMU: `apt install qemu-system-arm`.

The x86 build is unchanged: `cmake -S . -B build` as before.

## On the real board

1. FAT32-format an SD card (one partition).
2. Copy `bootcode.bin`, `start.elf` and `fixup.dat` from the `boot/` folder
   of https://github.com/raspberrypi/firmware.
3. Copy everything in `bin/rpi3-boot/` (`kernel8.img`, `config.txt`).
4. Wire a 3.3 V USB serial adapter: GND to pin 6, adapter RX to pin 8
   (GPIO 14, TXD), adapter TX to pin 10 (GPIO 15, RXD). 115200 8N1.
   Never a 5 V adapter.
5. Power on. The banner should name "Pi 3 Model B+" (revision 0xa020d3).

## Layout

| Path | What |
|---|---|
| `kernel/arch/cpu.h` | CPU primitives for portable code (irq on/off/save/restore, halt, store fence). Picks `arch/x86/cpu.h` or `arch/aarch64/cpu.h` |
| `kernel/arch/vmm.h` | Page allocator header for portable code (x86 only so far) |
| `kernel/arch/aarch64/` | CPU layer: `cpu.h`, `mmio.h`, `vectors.S` + `exception.c` (the `boot/x86/isr/` counterpart), `scheduler/sched_aarch64.c` |
| `kernel/boot/rpi3/` | Board: `start.S` (EL2 to EL1, stack, .bss), `linker.ld` (0x80000), `board.h` (addresses), PL011 `uart.c`, `mailbox.c`, `boot.c` (kmain), `config.txt` |
| `kernel/scheduler/sched_platform.h` | What the scheduler needs from the CPU. x86: `arch/x86/scheduler/sched_x86.c` |
| `cmake/` | `x86.cmake` (the old CMakeLists body), `rpi3.cmake`, `toolchain-aarch64.cmake` |

`KERNEL_PORTABLE_C_SOURCES` in `CMakeLists.txt` lists the files both targets
compile. A file moves there once it no longer depends on x86.

## The arch boundary

Portable code does not include `kernel/arch/x86/...` or write inline asm.
It goes through one of two kinds of interface:

* **Small CPU primitives** in `kernel/arch/cpu.h`, as static inline
  functions each arch implements the same way.
* **A subsystem's platform layer**: a `<subsystem>/<name>_platform.h` header
  declaring what the subsystem needs, implemented per arch. IPC already did
  this (`ipc/ipc_platform.h`, `ipc_x86.c`); the scheduler now does too.
  New implementations go under `kernel/arch/<arch>/`, so an ARM port adds a
  directory instead of adding `#ifdef`s across the tree.

## Scheduler

Policy in `scheduler.c` (run queue, priorities, aging, sleep, events,
picking the next thread) was always CPU independent. The x86 parts, now in
`arch/x86/scheduler/sched_x86.c` behind `sched_platform.h`, were:

* the initial register frame for kernel and user threads (EIP, CS, EFLAGS,
  segment selectors, user ESP/SS)
* the FXSAVE default image and the boot thread's FPU area
* CR3 reads and writes, and the TSS `esp0` update on every switch
* `int $0x80` as the yield, `hlt` in the idle thread, `cli` in thread exit

`sched_aarch64.c` implements the same calls with `svc`, `wfi`, an EL1h/EL0t
`SPSR` frame matching `vectors.S`, and TTBR0. It compiles and links into the
stub but runs once `scheduler.c` joins the rpi3 build, which needs the vmm
and heap first.

Still x86 inside the scheduler directory:

* `ThreadBlock.esp`, `cr3`, `kernel_stack_top` and the entry/stack arguments
  are `uint32_t`. Fine on a Pi 3 with RAM identity-mapped below 1 GB, but
  they should become `uintptr_t`. The x86 asm stubs read `fpu_state` at offset
  16, so change the layout and `TB_FPU_STATE` together.
* `process.c`: CR0/CR3 access and the x86 page directory format, used to
  build a process's address space. That goes behind a vmm interface.
* `watchdog.c`: `cpuid`/`rdmsr` (APIC base), the IOAPIC NMI and reading
  `%esp`. That becomes a `watchdog_platform.h`.

## Next steps, in boot order (see `boot.c`)

1. `mmu_init()`: identity map RAM (Normal, cached) and peripherals (Device).
   After that, drop `-mstrict-align`.
2. Interrupts: BCM2836 local controller (core timer) and the legacy GPU
   controller at 0x3F00B200, a 1 kHz tick into `thread_handler_c`.
3. pmm / vmm / heap under `kernel/arch/aarch64/virtual/`, then add
   `scheduler.c`, the heap, `knode.c` and friends to the rpi3 build.
4. Framebuffer over the mailbox, then `display_init` / `draw_set_*`.
5. Storage: EMMC (SD card) as a block device; FAT already exists.
6. Input: DWC2 USB host + the existing HID code (no PS/2 on a Pi).
