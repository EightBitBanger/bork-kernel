#ifndef ICON_IMAGE_LIBRARY_H
#define ICON_IMAGE_LIBRARY_H

#include <kernel/dwm/configuration.h>

#ifdef ADD_IMAGE_LIB

#include <stdint.h>
#include <kernel/arch/x86/drivers/display/draw.h>

extern const struct Sprite rc_icon_storage;
extern const struct Sprite rc_icon_system;
extern const struct Sprite rc_icon_folder;
extern const struct Sprite rc_icon_system;
extern const struct Sprite rc_icon_document;
extern const struct Sprite rc_icon_file;

#endif

#endif
