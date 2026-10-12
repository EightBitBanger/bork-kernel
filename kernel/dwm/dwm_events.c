#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/util/list.h>

void dwm_process_context_menu_events(struct WindowContext* ctx, uint16_t index) {
    switch (ctxmenu.menu_directive) {
    case DWM_CONTEXT_MENU_USER:
        if (ctxmenu.handle != NULL) {
            dwm_post_message(ctxmenu.handle->id, DWM_EVENT_CONTEXT_MENU, index, 0);
        }
        break;
        
    case DWM_CONTEXT_MENU_DESKTOP:
        dwm_desktop_context_callback(ctx, index);
        break;
        
    case DWM_CONTEXT_MENU_ICON:
        dwm_icon_context_callback(ctx, index);
        break;
    }
}
