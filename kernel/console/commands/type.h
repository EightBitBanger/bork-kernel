#ifndef CONSOLE_CMD_TYPE_H
#define CONSOLE_CMD_TYPE_H

#include <stdint.h>
#include <string.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>

// Usage:
//   type <file>                  Print text file to console
//   type <file> > <dest>         Clear/overwrite destination file with source data
//   type <file> >> <dest>        Append source data to destination file
static int cmd_type_run(int arg_count, char** args) {
    if (arg_count == 0) {
        return 1;
    }
    
    // Resolve source file path
    char src_path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], src_path, sizeof(src_path))) {
        return 3;
    }
    
    File src_file = vfs_open(src_path, VFS_OPEN_READ);
    if (src_file == VFS_INVALID_FILE) {
        return 2;
    }
    
    // Check for redirection arguments (> or >>)
    int redirect_mode = 0; // 0 = none, 1 = overwrite (>), 2 = append (>>)
    char dest_path[VFS_PATH_MAX];
    
    if (arg_count >= 3) {
        if (strcmp(args[1], ">") == 0) {
            redirect_mode = 1;
        } else if (strcmp(args[1], ">>") == 0) {
            redirect_mode = 2;
        }
        
        if (redirect_mode > 0) {
            if (!command_resolve_path(args[2], dest_path, sizeof(dest_path))) {
                vfs_close(src_file);
                return 4;
            }
        }
    }
    
    // Open destination file based on redirection mode
    File dest_file = VFS_INVALID_FILE;
    if (redirect_mode == 1) {
        // Open for writing, create if missing, truncate (clear) if it exists
        dest_file = vfs_open(dest_path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE);
    } else if (redirect_mode == 2) {
        // Open for writing, create if missing, append to the end
        dest_file = vfs_open(dest_path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_APPEND);
    }
    
    if (redirect_mode > 0 && dest_file == VFS_INVALID_FILE) {
        vfs_close(src_file);
        return 5;
    }
    
    // Read and process in chunks
    char buffer[129];
    uint64_t remaining = vfs_get_size(src_file);
    
    while (remaining > 0) {
        uint32_t want = (remaining < sizeof(buffer) - 1) ? (uint32_t)remaining : (uint32_t)(sizeof(buffer) - 1);
        int got = vfs_read(src_file, buffer, want);
        if (got <= 0)
            break;
        
        buffer[got] = '\0';
        
        if (redirect_mode > 0) {
            // Pipe output into destination file
            int written = vfs_write(dest_file, buffer, got);
            if (written < 0) {
                break;
            }
        } else {
            // Default behavior: print to console
            print(buffer);
        }
        
        // A NUL byte ends the text, as before
        if (strlen(buffer) < (size_t)got)
            break;
        remaining -= (uint32_t)got;
    }
    
    // Cleanup and close files
    vfs_close(src_file);
    if (redirect_mode > 0) {
        vfs_close(dest_file);
    }
    
    return 0;
}

int console_cmd_type(int arg_count, char** args) {
    int result = cmd_type_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
