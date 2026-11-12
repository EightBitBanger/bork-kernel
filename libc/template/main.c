#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("%s %s, pid %d\n", APP_NAME, APP_VERSION, getpid());
    return 0;
}

