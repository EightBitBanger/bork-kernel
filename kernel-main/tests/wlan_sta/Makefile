# Host test for the Wi-Fi station: make (builds and runs it)

ROOT   := ../..
CRYPT  := $(ROOT)/kernel/crypt
STA    := $(ROOT)/kernel/arch/x86/drivers/net/wlan_sta.c
CFLAGS := -std=gnu99 -O1 -g -Wall -Wextra -Werror -I../crypt/shim -I$(ROOT) -fsanitize=address,undefined

build/sta_test: sta_test.c $(STA) $(STA:.c=.h) $(wildcard $(CRYPT)/*.c) $(wildcard $(CRYPT)/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) sta_test.c $(STA) $(wildcard $(CRYPT)/*.c) -o $@
	./$@

.PHONY: clean
clean:
	rm -rf build
