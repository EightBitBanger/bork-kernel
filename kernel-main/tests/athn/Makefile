# Host test for the AR9485 driver and the station over it: make (builds and runs it)

ROOT   := ../..
NET    := $(ROOT)/kernel/arch/x86/drivers/net
ATHN   := $(ROOT)/drivers/athn-pci
CRYPT  := $(ROOT)/kernel/crypt
CFLAGS := -std=gnu99 -O1 -g -Wall -Wextra -DKERNEL_PLATFORM_X86 -DATHN_HOST_TEST -I$(ROOT) -I$(ROOT)/kernel \
          -fsanitize=address,undefined -fno-sanitize-recover=all

build/athn_test: athn_test.c $(wildcard $(NET)/*.c) $(wildcard $(NET)/*.h) $(wildcard $(ATHN)/*) $(wildcard $(CRYPT)/*.c) \
                 $(wildcard $(CRYPT)/*.h) $(ROOT)/kernel/driver/device.c
	mkdir -p build
	$(CC) $(CFLAGS) athn_test.c $(NET)/wlan_sta.c $(wildcard $(CRYPT)/*.c) -o $@
	./$@

.PHONY: clean
clean:
	rm -rf build
