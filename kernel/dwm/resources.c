#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/util/string.h>

#include <kernel/util/list.h>
#include <kernel/util/map.h>

void dwm_resource_load(const char* name, void* resource) {
    DWM_GUARD();
    if (name == NULL || resource == NULL) return;
    
    if (map_append(&workspace.resource_head, &workspace.resource_tail, resource)) {
        strncpy(workspace.resource_tail->name, name, sizeof(workspace.resource_tail->name) - 1);
        workspace.resource_tail->name[sizeof(workspace.resource_tail->name) - 1] = '\0';
    }
}

void* dwm_resource_find(const char* name) {
    DWM_GUARD();
    if (name == NULL) return NULL;
    
    struct map_node* current = workspace.resource_head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            return current->data;
        }
        current = current->next;
    }
    
    return NULL;
}

void dwm_resource_unload(const char* name) {
    DWM_GUARD();
    if (name == NULL) return;
    
    struct map_node* current = workspace.resource_head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            map_remove(&workspace.resource_head, &workspace.resource_tail, current->data);
            return;
        }
        current = current->next;
    }
}

// Window resource management

uint8_t dwm_window_resource_add(WindowHandle handle, const char* name, void* resource) {
    DWM_GUARD();
    if (resource == NULL || name == NULL) return 0;
    
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return 0;
    
    // Cast/use the map pointers now stored in the window object
    if (map_append((struct map_node**)&window->resource_head, (struct map_node**)&window->resource_tail, resource)) {
        struct map_node* tail = (struct map_node*)window->resource_tail;
        strncpy(tail->name, name, sizeof(tail->name) - 1);
        tail->name[sizeof(tail->name) - 1] = '\0';
        return 1;
    }
    
    return 0;
}

uint8_t dwm_window_resource_remove(WindowHandle handle, void* resource) {
    DWM_GUARD();
    if (resource == NULL) return 0;
    
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return 0;
    
    if (map_remove((struct map_node**)&window->resource_head, (struct map_node**)&window->resource_tail, resource)) {
        return 1;
    }
    
    return 0;
}

uint32_t dwm_window_resource_get_count(WindowHandle handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return 0;
    
    uint32_t count = 0;
    for (struct map_node* node = (struct map_node*)window->resource_head; node != NULL; node = node->next) {
        count++;
    }
    
    return count;
}

// Kept index lookup as requested
void* dwm_window_resource_get_by_index(WindowHandle handle, uint32_t index) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return NULL;
    
    uint32_t current_index = 0;
    for (struct map_node* node = (struct map_node*)window->resource_head; node != NULL; node = node->next) {
        if (current_index == index) {
            return node->data;
        }
        current_index++;
    }
    
    return NULL;
}

void* dwm_window_resource_get_by_name(WindowHandle handle, const char* name) {
    DWM_GUARD();
    if (name == NULL) return NULL;
    
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return NULL;
    
    for (struct map_node* node = (struct map_node*)window->resource_head; node != NULL; node = node->next) {
        if (strncmp(node->name, name, DWM_MAX_NAME_LEN) == 0) {
            return node->data;
        }
    }
    
    return NULL;
}

void dwm_window_resource_free_all(WindowHandle handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return;
    
    while (window->resource_head != NULL) {
        struct map_node* res_node = (struct map_node*)window->resource_head;
        void* resource_data = res_node->data;
        
        map_remove((struct map_node**)&window->resource_head, (struct map_node**)&window->resource_tail, resource_data);
        
        if (resource_data != NULL) {
            free(resource_data);
        }
    }
}
