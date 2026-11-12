#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

int main(void) {
    printf("hello from user space, pid %d\n", getpid());
    
    // Heap
    char* msg = malloc(64);
    snprintf(msg, 64, "%-8s|%5.2f|%#x", "libc", 3.14159, 255);
    puts(msg);
    free(msg);
    
    // Files
    FILE* f = fopen("hello.txt", "w");
    if (f == NULL) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "written by %s\n", "fprintf");
    fclose(f);

    char line[128];
    f = fopen("hello.txt", "r");
    if (f && fgets(line, sizeof(line), f))
        printf("read back: %s", line);
    if (f) fclose(f);
    remove("hello.txt");

    // Directory listing of the working directory
    char cwd[256];
    printf("contents of %s:\n", getcwd(cwd, sizeof(cwd)));
    DIR* d = opendir(".");
    struct dirent* e;
    while (d && (e = readdir(d)) != NULL)
        printf("  %s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
    if (d) closedir(d);

    return 0;
}

