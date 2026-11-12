#ifndef DWM_SEARCH_H
#define DWM_SEARCH_H

/*
 * File and folder search for the start menu (taskbar.h). Not for programs.
 *
 *     search_begin("note txt");           // names holding "note" and "txt"
 *     while (search_busy())
 *         search_step(SEARCH_STEP_MS);     // once per main loop round
 *     for (size_t i = 0; i < search_count(); i++) ... search_hit(i)
 *
 * The roots are walked breadth first, so shallow matches come first: one
 * folder at a time is read, its matching entries kept and its subfolders
 * queued, until the round's time is used up. Matching ignores case; every
 * word of the query must be in the name. "." and "..", links and anything
 * unusual are left out. A folder that can't be read is skipped quietly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEARCH_QUERY_MAX    128         // Bytes, terminator included
#define SEARCH_MAX_HITS     200         // Kept; more are counted (search_more)
#define SEARCH_MAX_QUEUED   4096        // Folders waiting; deeper ones are left out
#define SEARCH_MAX_DEPTH    24          // Below a root
#define SEARCH_MAX_ROOTS    8
#define SEARCH_STEP_MS      6           // A round's share of the walk

typedef struct {
    char* path;                         // Absolute
    bool  folder;
} search_hit_t;

// Folders to walk (copied; NULL / 0: "/mnt"). A search going on restarts.
void   search_set_roots(const char* const* roots, size_t count);

// Start over with `query` (empty: nothing found, not busy)
void   search_begin(const char* query);
void   search_cancel(void);             // Stop, and forget the hits

bool   search_busy(void);
// Walk on for about `budget_ms`. True when hits were added (or it finished).
bool   search_step(uint32_t budget_ms);

size_t              search_count(void);
const search_hit_t* search_hit(size_t index);
size_t              search_more(void);          // Matches past SEARCH_MAX_HITS
size_t              search_folders(void);       // Folders read so far
bool                search_partial(void);       // Some folders were left out (too many, too deep)

#endif
