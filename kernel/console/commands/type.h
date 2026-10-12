#ifndef CONSOLE_CMD_TYPE_H
#define CONSOLE_CMD_TYPE_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>

// type <file>   print a text file
int console_cmd_type(int arg_count, char** args) {
    if (arg_count == 0) 
        return 1;
    
    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 3;
    
    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) 
        return 2;
    
    // Read and print in chunks: one byte per vfs_read() and one print()
    // per character was very slow for anything but short files
    char buffer[129];
    uint32_t remaining = vfs_get_size(file);
    
    while (remaining > 0) {
        uint32_t want = (remaining < sizeof(buffer) - 1) ? remaining : (uint32_t)(sizeof(buffer) - 1);
        int got = vfs_read(file, buffer, want);
        if (got <= 0)
            break;
        
        buffer[got] = '\0';
        print(buffer);
        
        // A NUL byte ends the text, as before
        if (strlen(buffer) < (size_t)got)
            break;
        remaining -= (uint32_t)got;
    }
    
    vfs_close(file);
    return 0;
}

#endif
