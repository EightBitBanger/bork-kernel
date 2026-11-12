/*
 * File and folder search (search.h).
 *
 * The folders still to read are a queue of malloc'd paths (breadth first);
 * the hits a fixed table of malloc'd paths. search_step() reads whole
 * folders, one after another, until its time is up: a folder is the
 * smallest piece of work, so a huge one can make one round slower.
 */

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "search.h"
#include "timer.h"

#define DEFAULT_ROOT    "/mnt"
#define WORDS_MAX       8

typedef struct {
    char*   path;
    uint8_t depth;
} queued_t;

static char*        roots[SEARCH_MAX_ROOTS];
static size_t       root_count;

static char         query[SEARCH_QUERY_MAX];
static char         words[WORDS_MAX][SEARCH_QUERY_MAX];     // Lower case
static size_t       word_count;

static queued_t*    queue;
static size_t       queue_head, queue_tail, queue_cap;

static search_hit_t hits[SEARCH_MAX_HITS];
static size_t       hit_count;
static size_t       more;
static size_t       folders;
static bool         partial;
static bool         busy;

//
// Queue
//

static void queue_clear(void) {
    for (size_t i = queue_head; i < queue_tail; i++)
        free(queue[i].path);
    queue_head = queue_tail = 0;
}

static bool queue_push(const char* path, uint8_t depth) {
    if (queue_tail - queue_head >= SEARCH_MAX_QUEUED) {
        partial = true;
        return false;
    }
    if (queue_tail == queue_cap) {
        if (queue_head > 0) {
            // Slide what is left to the front before growing
            memmove(queue, queue + queue_head, (queue_tail - queue_head) * sizeof(*queue));
            queue_tail -= queue_head;
            queue_head = 0;
        }
        if (queue_tail == queue_cap) {
            size_t grow = queue_cap ? queue_cap * 2 : 64;
            queued_t* q = (queued_t*)realloc(queue, grow * sizeof(*q));
            if (q == NULL) {
                partial = true;
                return false;
            }
            queue = q;
            queue_cap = grow;
        }
    }
    char* copy = strdup(path);
    if (copy == NULL) {
        partial = true;
        return false;
    }
    queue[queue_tail].path  = copy;
    queue[queue_tail].depth = depth;
    queue_tail++;
    return true;
}

//
// Matching
//

static void lower_copy(char* out, const char* in, size_t size) {
    size_t i = 0;
    for (; in[i] != '\0' && i + 1 < size; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = '\0';
}

static bool matches(const char* name) {
    char lower[NAME_MAX + 1];
    lower_copy(lower, name, sizeof(lower));
    for (size_t i = 0; i < word_count; i++) {
        if (strstr(lower, words[i]) == NULL)
            return false;
    }
    return true;
}

static void add_hit(const char* path, bool folder) {
    if (hit_count == SEARCH_MAX_HITS) {
        more++;
        return;
    }
    char* copy = strdup(path);
    if (copy == NULL) {
        more++;
        return;
    }
    hits[hit_count].path   = copy;
    hits[hit_count].folder = folder;
    hit_count++;
}

static void hits_clear(void) {
    for (size_t i = 0; i < hit_count; i++)
        free(hits[i].path);
    hit_count = 0;
    more = 0;
}

//
// Public
//

void search_set_roots(const char* const* list, size_t count) {
    for (size_t i = 0; i < root_count; i++)
        free(roots[i]);
    root_count = 0;
    for (size_t i = 0; list != NULL && i < count && root_count < SEARCH_MAX_ROOTS; i++) {
        if (list[i] != NULL && list[i][0] == '/' && (roots[root_count] = strdup(list[i])) != NULL)
            root_count++;
    }
    if (busy) {
        char again[SEARCH_QUERY_MAX];
        strlcpy(again, query, sizeof(again));
        search_begin(again);
    }
}

void search_cancel(void) {
    queue_clear();
    hits_clear();
    folders = 0;
    partial = false;
    busy    = false;
    query[0] = '\0';
    word_count = 0;
}

void search_begin(const char* text) {
    search_cancel();
    if (text == NULL)
        return;
    strlcpy(query, text, sizeof(query));

    // Words, lower case
    const char* p = query;
    while (*p != '\0' && word_count < WORDS_MAX) {
        while (*p == ' ')
            p++;
        size_t n = 0;
        while (p[n] != '\0' && p[n] != ' ')
            n++;
        if (n == 0)
            break;
        for (size_t i = 0; i < n; i++)
            words[word_count][i] = (char)tolower((unsigned char)p[i]);
        words[word_count][n] = '\0';
        word_count++;
        p += n;
    }
    if (word_count == 0)
        return;

    if (root_count == 0)
        queue_push(DEFAULT_ROOT, 0);
    for (size_t i = 0; i < root_count; i++)
        queue_push(roots[i], 0);
    busy = true;
}

bool search_busy(void) {
    return busy;
}

// Read one folder: matching entries become hits, subfolders are queued
static void read_folder(const queued_t* q) {
    DIR* d = opendir(q->path);
    if (d == NULL)
        return;             // Gone, or not ours to read: quietly left out
    folders++;

    bool root = (strcmp(q->path, "/") == 0);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR && e->d_type != DT_REG)
            continue;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char path[PATH_MAX];
        size_t len = strlcpy(path, root ? "" : q->path, sizeof(path));
        if (len + 1 + strlen(e->d_name) >= sizeof(path)) {
            partial = true;
            continue;
        }
        path[len] = '/';
        strlcpy(path + len + 1, e->d_name, sizeof(path) - len - 1);

        bool folder = (e->d_type == DT_DIR);
        if (matches(e->d_name))
            add_hit(path, folder);
        if (folder) {
            if (q->depth + 1 > SEARCH_MAX_DEPTH)
                partial = true;
            else
                queue_push(path, (uint8_t)(q->depth + 1));
        }
    }
    closedir(d);
}

bool search_step(uint32_t budget_ms) {
    if (!busy)
        return false;
    size_t before = hit_count + more;
    uint64_t start = dwm_time_ms();
    do {
        if (queue_head == queue_tail) {
            busy = false;
            return true;    // Done: "searching" goes away
        }
        queued_t q = queue[queue_head++];
        read_folder(&q);
        free(q.path);
    } while (dwm_time_ms() - start < budget_ms);
    return hit_count + more != before;
}

size_t search_count(void) {
    return hit_count;
}

const search_hit_t* search_hit(size_t index) {
    return index < hit_count ? &hits[index] : NULL;
}

size_t search_more(void)    { return more; }
size_t search_folders(void) { return folders; }
bool   search_partial(void) { return partial; }
