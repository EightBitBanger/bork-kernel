# Host test for kernel/crypt: make (builds and runs it)

ROOT   := ../..
CRYPT  := $(ROOT)/kernel/crypt
CFLAGS := -std=gnu99 -O2 -Wall -Wextra -Werror -Ishim -I$(ROOT) -fsanitize=address,undefined

build/crypt_test: crypt_test.c $(wildcard $(CRYPT)/*.c) $(wildcard $(CRYPT)/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) crypt_test.c $(wildcard $(CRYPT)/*.c) -o $@
	./$@

.PHONY: clean
clean:
	rm -rf build
