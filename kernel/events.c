#include <stdio.h>
#include <stdbool.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

#include <kernel/util/list.h>
#include <kernel/device/kevent.h>
#include <kernel/events.h>
#include <kernel/mutex.h>

struct list_node* event_list_head = NULL;
struct list_node* event_list_tail = NULL;

static mutex_t event_mutex = {
    .locked = false,
    .owner = NULL,
    .wait_head = NULL,
    .wait_tail = NULL
};

enum EventAction {
    ACTION_NONE,
    ACTION_EXPLORER,
    ACTION_NOTEPAD
};

void kernel_event_init(void) {
    mutex_init(&event_mutex);
}

bool kernel_event_send(uint8_t flags, const char* name, const char* arguments) {
    // Allocate and copy data outside the lock to minimize critical section time
    struct KEvent* new_event = malloc(sizeof(struct KEvent));
    if (new_event == NULL)
        return false;

    // Initialize the KEvent fields and guarantee null-termination
    strncpy(new_event->name, name, KEVENT_NAME_LENGTH_MAX);
    new_event->name[KEVENT_NAME_LENGTH_MAX - 1] = '\0';

    strncpy(new_event->args, arguments, KEVENT_ARG_LENGTH_MAX);
    new_event->args[KEVENT_ARG_LENGTH_MAX - 1] = '\0';

    new_event->flags = flags;

    // Protect shared list mutation
    mutex_lock(&event_mutex);
    bool success = list_append(&event_list_head, &event_list_tail, (void*)new_event);
    mutex_unlock(&event_mutex);

    if (!success) {
        free(new_event);
        return false;
    }

    return true;
}

// Internal unlocked helper (caller must hold event_mutex)
static bool kernel_event_remove_by_ptr_unlocked(struct KEvent* target_event) {
    if (target_event == NULL) return false;

    if (!list_remove(&event_list_head, &event_list_tail, (void*)target_event)) {
        return false;
    }

    free(target_event);
    return true;
}

bool kernel_event_remove_by_ptr(struct KEvent* target_event) {
    if (target_event == NULL) return false;

    mutex_lock(&event_mutex);
    bool result = kernel_event_remove_by_ptr_unlocked(target_event);
    mutex_unlock(&event_mutex);

    return result;
}

bool kernel_event_remove(const char* name) {
    mutex_lock(&event_mutex);

    if (event_list_head == NULL) {
        mutex_unlock(&event_mutex);
        return false;
    }

    struct list_node* current = event_list_head;

    while (current != NULL) {
        struct KEvent* ev = (struct KEvent*)current->data;
        if (strncmp(ev->name, name, KEVENT_NAME_LENGTH_MAX) == 0) {
            bool result = kernel_event_remove_by_ptr_unlocked(ev);
            mutex_unlock(&event_mutex);
            return result;
        }
        current = current->next;
    }

    mutex_unlock(&event_mutex);
    return false;
}

void kernel_event_update(void) {
    enum EventAction action = ACTION_NONE;
    char args_buffer[KEVENT_ARG_LENGTH_MAX] = {0};

    mutex_lock(&event_mutex);

    struct list_node* current = event_list_head;

    while (current != NULL) {
        struct list_node* next_node = current->next;
        struct KEvent* event = (struct KEvent*)current->data;

        // Execute program
        if (event->flags & KEVENT_EXECUTE) {
            event->flags &= ~KEVENT_EXECUTE;

            // Copy parameters locally to safely execute outside the lock
            strncpy(args_buffer, event->args, KEVENT_ARG_LENGTH_MAX);
            args_buffer[KEVENT_ARG_LENGTH_MAX - 1] = '\0';

            if (strncmp(event->name, "explorer", KEVENT_NAME_LENGTH_MAX) == 0) {
                action = ACTION_EXPLORER;
            } else if (strncmp(event->name, "notepad", KEVENT_NAME_LENGTH_MAX) == 0) {
                action = ACTION_NOTEPAD;
            }

            event->flags |= KEVENT_DEAD;
            break; // Stop scanning after finding an execution task
        }

        // Unknown or unhandled event
        event->flags |= KEVENT_DEAD;
        current = next_node;
    }

    mutex_unlock(&event_mutex);

    if (action == ACTION_EXPLORER) {
        
        
    } else if (action == ACTION_NOTEPAD) {
        
        
    }

    mutex_lock(&event_mutex);

    struct list_node* dead = event_list_head;
    while (dead != NULL) {
        struct list_node* next_node = dead->next;
        struct KEvent* event = (struct KEvent*)dead->data;

        if (event->flags & KEVENT_DEAD) {
            list_remove(&event_list_head, &event_list_tail, (void*)event);
            free(event);
        }

        dead = next_node;
    }

    mutex_unlock(&event_mutex);
}

