
# Host tests. The libc is rebuilt with every syscall routed through
# linux_shim.c (an emulation of the kernel's syscalls on 32-bit Linux), and
# the test programs are linked exactly like real user programs: crt0.o,
# user.ld, libc.a.

CC ?= gcc
ifeq ($(origin CC),default)
CC := gcc
endif

ROOT  := ..
GCC_INCLUDE := $(shell $(CC) -print-file-name=include)
ARCH  := -m32 -march=i686

# GCC only: stop it turning the byte loops in string.c into memcpy/memset
# calls (which would recurse). Clang has no such flag and doesn't need it.
NO_LOOP_PATTERNS := $(shell $(CC) -fno-tree-loop-distribute-patterns -E -x c /dev/null >/dev/null 2>&1 && echo " -fno-tree-loop-distribute-patterns")
LIBC_CFLAGS := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -nostdinc \
               -isystem $(GCC_INCLUDE) -I$(ROOT)/include -fno-pic -fno-pie -fno-stack-protector \
               -fcf-protection=none -fno-asynchronous-unwind-tables$(NO_LOOP_PATTERNS) \
               -DLIBC_SYSCALL_HOOK=__test_syscall
USER_CFLAGS := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -Wno-format -ffreestanding -nostdinc \
               -isystem $(GCC_INCLUDE) -I$(ROOT)/include -fno-pic -fno-pie -fno-stack-protector \
               -fcf-protection=none -fno-asynchronous-unwind-tables -DLIBC_SYSCALL_HOOK=__test_syscall
LDFLAGS := $(ARCH) -nostdlib -static -no-pie -T $(ROOT)/user.ld -Wl,--build-id=none

SRCS := errno.c abort.c process.c unistd.c string.c ctype.c divdi3.c malloc.c stdlib.c start.c printf.c stdio.c dirent.c gfx.c security.c ipc.c resource.c time.c wm.c font.c math.c ui.c filetype.c registry.c inttypes.c setjmp.c \
         image.c image_png.c stbimage.c stbimagewrite.c stbtruetype.c ulib.c
OBJS := $(addprefix build/,$(SRCS:.c=.o)) build/linux_shim.o

# Graphics tests run the kernel's own gfx code and command interpreter, so
# they need the kernel sources (gfx/, syscall/, arch/...). By default they
# are expected one level above this libc folder; point KERNEL_SRC elsewhere
# if not. Without them the graphics test is skipped.
KERNEL_SRC ?= $(ROOT)/..
HAVE_KERNEL := $(wildcard $(KERNEL_SRC)/gfx/gfx.h $(KERNEL_SRC)/syscall/calls/sys_gfx_core.h)
KINC := build/kinc/kernel
KERNEL_GFX := gfx_draw gfx_surface gfx_text
ifeq ($(words $(HAVE_KERNEL)),2)
OBJS += build/kgfx.o
GFX_TEST := build/gfx_test build/resource_test
endif
# IPC tests run the kernel's ipc/ipc.c the same way
HAVE_KIPC := $(wildcard $(KERNEL_SRC)/ipc/ipc.c)
ifneq ($(HAVE_KIPC),)
OBJS += build/kipc.o
IPC_TEST := build/ipc_test
ifeq ($(words $(HAVE_KERNEL)),2)
IPC_TEST += build/input_demo_test
endif
endif
KFLAGS := -Ikstub -Ibuild/kinc

# Window server test: the DWM's own sources (src/dwm) next to this libc
DWM_SRC ?= $(ROOT)/../src
HAVE_DWM := $(wildcard $(DWM_SRC)/dwm/server.c)
DWM_PARTS := $(filter-out dwm,$(basename $(notdir $(wildcard $(DWM_SRC)/dwm/*.c))))
ifneq ($(HAVE_KIPC),)
ifneq ($(HAVE_DWM),)
# (wm_test.c still asks server.c for test hooks it no longer has, so it is
# left out until it is brought up to date: make build/wm_test to see)
WM_TEST := build/tray_test build/nettray_test build/menu_test
endif
endif

all: run

build:
	mkdir -p build

build/%.o: $(ROOT)/src/%.c | build
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

build/linux_shim.o: linux_shim.c | build
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

# Kernel headers the gfx code includes, laid out as <kernel/...>
build/kinc.stamp: | build
	mkdir -p $(KINC)/gfx $(KINC)/syscall/calls $(KINC)/arch/x86/drivers/display
	cp $(KERNEL_SRC)/gfx/*.h $(KINC)/gfx/
	cp $(KERNEL_SRC)/syscall/calls/sys_gfx_core.h $(KINC)/syscall/calls/
	cp $(KERNEL_SRC)/arch/x86/drivers/display/char_rom.h $(KINC)/arch/x86/drivers/display/
	touch $@

build/k_%.o: $(KERNEL_SRC)/gfx/%.c build/kinc.stamp
	$(CC) $(LIBC_CFLAGS) -Wno-sign-compare $(KFLAGS) -c $< -o $@

# The kernel's IPC core, its headers as <kernel/ipc/...>
build/kipc_inc.stamp: | build
	mkdir -p $(KINC)/ipc
	cp $(KERNEL_SRC)/ipc/*.h $(KINC)/ipc/
	mkdir -p $(KINC)/util && cp $(KERNEL_SRC)/util/nospec.h $(KINC)/util/
	touch $@

# Its malloc / free are counted, so the test can see that nothing leaks
KIPC_COUNT := -Dmalloc=__test_ipc_malloc -Dfree=__test_ipc_free -Drealloc=__test_ipc_realloc

build/k_ipc.o: $(KERNEL_SRC)/ipc/ipc.c build/kipc_inc.stamp
	$(CC) $(LIBC_CFLAGS) $(KFLAGS) $(KIPC_COUNT) -c $< -o $@

build/ipc_shim.o: ipc_shim.c build/kipc_inc.stamp
	$(CC) $(LIBC_CFLAGS) $(KFLAGS) -c $< -o $@

# ipc_sys_* / ipc_plat_* stay private, like the gfx code: only the shim's
# __test_* entry points are global
build/kipc.o: build/ipc_shim.o build/k_ipc.o
	$(LD) -m elf_i386 -r $^ -o $@
	objcopy --wildcard --keep-global-symbol='__test_*' $@

build/gfx_shim.o: gfx_shim.c build/kinc.stamp
	$(CC) $(LIBC_CFLAGS) -DKERNEL_PLATFORM_X86 $(KFLAGS) -c $< -o $@

# The kernel's gfx_* functions share their names with the libc's: link the
# kernel side into one object and keep only the shim's entry points global
build/kgfx.o: build/gfx_shim.o $(addprefix build/k_,$(addsuffix .o,$(KERNEL_GFX)))
	$(LD) -m elf_i386 -r $^ -o $@
	objcopy --wildcard --keep-global-symbol='__test_gfx_*' $@

build/crt0.o: $(ROOT)/src/crt0.S | build
	$(CC) $(ARCH) -c $< -o $@

build/libc_test.a: $(OBJS)
	rm -f $@ && ar rcs $@ $^

build/input_demo_test: ../examples/input_demo.c

build/dwm_%.o: $(DWM_SRC)/dwm/%.c | build
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/dwm_res_%.o: $(DWM_SRC)/resources/%.c | build
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/wm_test: wm_test.c build/libc_test.a build/crt0.o $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) build/dwm_res_imagelib.o build/dwm_res_fontlib.o
	$(CC) $(USER_CFLAGS) -I$(DWM_SRC) -c $< -o build/wm_test.o
	$(CC) $(LDFLAGS) build/crt0.o build/wm_test.o $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) \
	      build/dwm_res_imagelib.o build/dwm_res_fontlib.o build/libc_test.a -o $@

build/menu_test: menu_test.c build/libc_test.a build/crt0.o $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) build/dwm_res_imagelib.o build/dwm_res_fontlib.o
	$(CC) $(USER_CFLAGS) -I$(DWM_SRC) -c $< -o build/menu_test.o
	$(CC) $(LDFLAGS) build/crt0.o build/menu_test.o $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) \
	      build/dwm_res_imagelib.o build/dwm_res_fontlib.o build/libc_test.a -o $@

build/tray_test: tray_test.c build/libc_test.a build/crt0.o $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) build/dwm_res_imagelib.o build/dwm_res_fontlib.o
	$(CC) $(USER_CFLAGS) -I$(DWM_SRC) -c $< -o build/tray_test.o
	$(CC) $(LDFLAGS) build/crt0.o build/tray_test.o $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) \
	      build/dwm_res_imagelib.o build/dwm_res_fontlib.o build/libc_test.a -o $@

# The network tray service (programs/netsrv) on the DWM, with a pretend
# adapter in place of its wlan_dev.c
NETTRAY_SRC ?= $(ROOT)/../netsrv/src
NETTRAY_WRAP := -Wl,--wrap=wm_tray_add -Wl,--wrap=wm_tray_set_tip -Wl,--wrap=sec_drop -Wl,--wrap=wm_tray_set_panel

build/nettray_test: nettray_test.c $(NETTRAY_SRC)/nettray.c $(NETTRAY_SRC)/netlist.c build/libc_test.a build/crt0.o $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) build/dwm_res_imagelib.o build/dwm_res_fontlib.o
	$(CC) $(USER_CFLAGS) -I$(DWM_SRC) -I$(NETTRAY_SRC) -c $< -o build/nettray_test.o
	$(CC) $(USER_CFLAGS) -I$(NETTRAY_SRC) -c $(NETTRAY_SRC)/nettray.c -o build/nettray.o
	$(CC) $(USER_CFLAGS) -I$(NETTRAY_SRC) -c $(NETTRAY_SRC)/netlist.c -o build/netlist.o
	$(CC) $(LDFLAGS) $(NETTRAY_WRAP) build/crt0.o build/nettray_test.o build/nettray.o build/netlist.o \
	      $(addprefix build/dwm_,$(addsuffix .o,$(DWM_PARTS))) \
	      build/dwm_res_imagelib.o build/dwm_res_fontlib.o build/libc_test.a -o $@

build/%: %.c build/libc_test.a build/crt0.o
	$(CC) $(USER_CFLAGS) -c $< -o build/$*.o
	$(CC) $(LDFLAGS) build/crt0.o build/$*.o build/libc_test.a -o $@

# The stb library (os/stblib.h): stblib_test.c with stb linked in (ref) and
# with libc_use_stblib's client, loading the real library file (dyn)
STBLIB_DIR := $(ROOT)/../stblib
# A TrueType font for the font part: the OS's own if it is next to
# programs/, else one from this machine (FONT=... to choose)
FONT ?= $(firstword $(wildcard $(abspath $(ROOT)/../../fonts/opensans.ttf) \
          /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/truetype/crosextra/Carlito-Regular.ttf \
          /usr/share/fonts/truetype/*/*.ttf))

build/stblib_client.o: $(ROOT)/stblib/stblib_client.c | build
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/stblib_ref: stblib_test.c build/libc_test.a build/crt0.o
	$(CC) $(USER_CFLAGS) -c $< -o build/stblib_test.o
	$(CC) $(LDFLAGS) build/crt0.o build/stblib_test.o build/libc_test.a -o $@

build/stblib_dyn: stblib_test.c build/stblib_client.o build/libc_test.a build/crt0.o
	$(CC) $(USER_CFLAGS) -DSTBLIB_DYN -c $< -o build/stblib_dyn.o
	$(CC) $(LDFLAGS) build/crt0.o build/stblib_dyn.o build/stblib_client.o build/libc_test.a -o $@
	@! nm $@ | grep -q 'stbi__\|stbtt__' || (echo "stblib_dyn has stb's own code linked in" && false)

build/stb: FORCE | build
	cd $(STBLIB_DIR) && bash ./build.sh
	cp $(STBLIB_DIR)/stb $@

stblib: build/stblib_ref build/stblib_dyn build/stb
	cd build && rm -rf stb_ref stb_dyn stb_none && mkdir -p stb_ref stb_dyn/sys/lib stb_none && cp stb stb_dyn/sys/lib/stb
	cd build/stb_ref && ../stblib_ref report $(FONT) > ../stblib_ref.txt
	cd build/stb_dyn && ../stblib_dyn report $(FONT) > ../stblib_dyn.txt
	diff -u build/stblib_ref.txt build/stblib_dyn.txt
	@echo "stblib: the library gives the same results as stb linked in ($$(wc -l < build/stblib_dyn.txt) lines$(if $(FONT),, - no font found, font part skipped))"
	cd build/stb_dyn && ../stblib_dyn loader
	cd build/stb_none && ../stblib_dyn missing

# Reference: the same case list through the host C library
build/fmt_cases_glibc: fmt_cases.c | build
	$(CC) -O2 -w $< -o $@

run: stblib build/fmt_cases build/fmt_cases_glibc build/run_test $(GFX_TEST) $(IPC_TEST) $(WM_TEST)
	./build/fmt_cases > build/fmt_ours.txt
	./build/fmt_cases_glibc > build/fmt_glibc.txt
	diff -u build/fmt_glibc.txt build/fmt_ours.txt && echo "printf/strtod/strtol: identical to glibc ($$(wc -l < build/fmt_ours.txt) lines)"
	cd build && rm -rf sandbox && mkdir sandbox && cd sandbox && ../run_test
ifdef GFX_TEST
	cd build && ./gfx_test
	cd build && rm -rf res && ./resource_test
else
	@echo "gfx: kernel sources not found at $(KERNEL_SRC) (set KERNEL_SRC=...), graphics test skipped"
endif
ifdef IPC_TEST
	cd build && ./ipc_test
ifeq ($(words $(HAVE_KERNEL)),2)
	cd build && ./input_demo_test
endif
else
	@echo "ipc: kernel sources not found at $(KERNEL_SRC) (set KERNEL_SRC=...), IPC test skipped"
endif
ifdef WM_TEST
	cd build && ./tray_test
	cd build && ./nettray_test
	cd build && ./menu_test
else
	@echo "wm: needs the kernel's ipc/ipc.c and the DWM sources at $(DWM_SRC) (set DWM_SRC=...), window server test skipped"
endif

clean:
	rm -rf build

.PHONY: all run clean stblib FORCE
FORCE:



