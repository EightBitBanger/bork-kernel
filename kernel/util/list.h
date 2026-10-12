#ifndef KERNEL_UTIL_LIST_H
#define KERNEL_UTIL_LIST_H

#include <stddef.h>
#include <stdbool.h>

#if defined(KERNEL_PLATFORM_X86)
  #include <kernel/arch/x86/heap.h>
#elif defined(KERNEL_PLATFORM_AVR)
  #include <kernel/arch/avr/heap.h>
#else
  #error "list.h: define KERNEL_PLATFORM_X86 or KERNEL_PLATFORM_AVR"
#endif

/*
 * Doubly linked list of void* payloads. The list owns its nodes, not the
 * data: list_remove frees the node only.
 */

struct list_node {
    void* data;
    struct list_node* next;
    struct list_node* prev;
};

static inline bool list_append(struct list_node** head, struct list_node** tail, void* data) {
    struct list_node* new_node = (struct list_node*)malloc(sizeof(struct list_node));
    if (new_node == NULL) return false;
    
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

static inline void list_unlink(struct list_node** head, struct list_node** tail, struct list_node* node) {
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

static inline bool list_remove(struct list_node** head, struct list_node** tail, void* target_data) {
    struct list_node* current = *head;
    
    while (current != NULL && current->data != target_data) {
        current = current->next;
    }
    
    if (current == NULL) return false;
    
    list_unlink(head, tail, current);
    free(current);
    return true;
}

#endif
