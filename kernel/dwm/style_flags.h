#ifndef WINDOW_STYLE_FLAGS_H
#define WINDOW_STYLE_FLAGS_H

#include <stdint.h>

#define DWM_WSTYLE_TOPMOST         0x0001   // Always on top of other windows
#define DWM_WSTYLE_NOBORDERS       0x0002   // Borderless window (good for fullscreen games)
#define DWM_WSTYLE_CHILD           0x0004   // Contained within a parent window
#define DWM_WSTYLE_RESIZEABLE      0x0008   // Thicker border that allows dragging to resize
#define DWM_WSTYLE_NOCLOSEBOX      0x0010   // Removes the close button
#define DWM_WSTYLE_CASCADE         0x0020   // Spawns window in a incrementally cascaded formation
#define DWM_WSTYLE_NOTITLEBAR      0x0040   // Keeps the border but removes the titlebar (and its buttons)

//
// Considering for implementation
/*

#define WINDOW_STYLE_POPUP           0x0008   // Temporary, dismissible window (like a context menu)
#define WINDOW_STYLE_MODAL           0x0010   // Blocks interaction with parent window until closed

#define WINDOW_STYLE_BORDER         0x0040 // Standard thin border
#define DWM_WSTYLE_RESIZEABLE     0x0080 // Thick border that allows dragging to resize
#define WINDOW_STYLE_TITLEBAR       0x0100 // Includes a title bar area for dragging
#define WINDOW_STYLE_SHADOW         0x0200 // Drops a shadow underneath the window

#define WINDOW_STYLE_MINIMIZEBOX    0x0400 // Enables the minimize button
#define WINDOW_STYLE_MAXIMIZEBOX    0x0800 // Enables the maximize button
#define WINDOW_STYLE_CLOSEBOX       0x1000 // Enables the close button

#define WINDOW_STYLE_VISIBLE        0x4000 // Starts visible (otherwise hidden until Show())
#define WINDOW_STYLE_MAXIMIZED      0x8000 // Starts maximized
#define WINDOW_STYLE_MINIMIZED      0x00010000 // Starts minimized to taskbar/dock
#define WINDOW_STYLE_DISABLED       0x00020000 // Starts in a grayed-out, unclickable state

*/

#endif
