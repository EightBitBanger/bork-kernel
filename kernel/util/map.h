#ifndef KERNEL_UTIL_MAP_H
#define KERNEL_UTIL_MAP_H

#include <stddef.h>
#include <stdbool.h>

#include <kernel/util/string.h>

#if defined(KERNEL_PLATFORM_X86)
  #include <kernel/arch/x86/heap.h>
#elif defined(KERNEL_PLATFORM_AVR)
  #include <kernel/arch/avr/heap.h>
#else
  #error "map.h: define KERNEL_PLATFORM_X86 or KERNEL_PLATFORM_AVR"
#endif

/*
 * Name-keyed doubly linked list. Names longer than MAP_NAME_MAX - 1 chars
 * are truncated. The map owns its nodes, not the data.
 */

#define MAP_NAME_MAX 16

struct map_node {
    char name[MAP_NAME_MAX];
    void* data;
    struct map_node* next;
    struct map_node* prev;
};

// Append an entry with the given name (NULL = empty name)
static inline bool map_insert(struct map_node** head, struct map_node** tail, const char* name, void* data) {
    struct map_node* new_node = (struct map_node*)malloc(sizeof(struct map_node));
    if (new_node == NULL) return false;
    
    new_node->name[0] = '\0';
    if (name != NULL) {
        strncpy(new_node->name, name, MAP_NAME_MAX - 1);
        new_node->name[MAP_NAME_MAX - 1] = '\0';
    }
    
    new_node->data = data;
    new_node->next = NULL;
    new_node->prev = *tail;
    
    if (*head == NULL) {
        *head = new_node;
    } else {
        (*tail)->next = new_node;
    }
    *tail = new_node;
    return true;
}

// Kept for compatibility: appends an entry with an empty name
static inline bool map_append(struct map_node** head, struct map_node** tail, void* data) {
    return map_insert(head, tail, NULL, data);
}

// Find the first node whose name matches (compared up to MAP_NAME_MAX - 1 chars)
static inline struct map_node* map_find(struct map_node* head, const char* name) {
    for (struct map_node* n = head; n != NULL; n = n->next) {
        if (strncmp(n->name, name, MAP_NAME_MAX - 1) == 0) return n;
    }
    return NULL;
}

static inline void map_unlink(struct map_node** head, struct map_node** tail, struct map_node* node) {
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        *head = node->next;
    }
    
    if (node->next != NULL) {
        node->next->prev = node->prev;
    } else {
        *tail = node->prev;
    }
}

static inline bool map_remove(struct map_node** head, struct map_node** tail, void* target_data) {
    struct map_node* current = *head;
    
    while (current != NULL && current->data != target_data) {
        current = current->next;
    }
    
    if (current == NULL) return false;
    
    map_unlink(head, tail, current);
    free(current);
    return true;
}

static inline bool map_remove_name(struct map_node** head, struct map_node** tail, const char* name) {
    struct map_node* node = map_find(*head, name);
    if (node == NULL) return false;
    
    map_unlink(head, tail, node);
    free(node);
    return true;
}

#endif
