/*
 * Prints the arguments it was started with. Launch it from spawn_demo, or
 * from the kernel (argc 1, argv[0] = its path).
 */

#include <stdio.h>

int main(int argc, char** argv) {
    printf("args: argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
