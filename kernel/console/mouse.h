
#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>

#define MOUSE_BUTTON_LEFT    0
#define MOUSE_BUTTON_MIDDLE  1
#define MOUSE_BUTTON_RIGHT   2

#define MOUSE_QUEUE_SIZE 256

typedef struct PointStruct {
    int32_t x;
    int32_t y;
} Point;

typedef struct RectangleStruct {
    int32_t x;
    int32_t y;
} Rect;

typedef struct {
    int32_t x;
    int32_t y;
    bool left_button;
    bool right_button;
    bool middle_button;
    int32_t wheel;          // Wheel notches since the last event: + toward the
                            // user (scroll down), - away (scroll up)
    uint64_t timestamp;
} MouseEvent;

void mouse_initiate(void);

void mouse_set_cursor_speed(int32_t horz, int32_t vert);
void mouse_set_cursor_acceleration(int32_t acceleration);

void mouse_event_handler(void);

// Relative motion (+y down) and buttons (1 left, 2 right, 4 middle) from
// any pointing device. Interrupts off.
void mouse_motion(int32_t rel_x, int32_t rel_y, uint8_t buttons);

// The same with wheel notches too (+ toward the user, i.e. scroll down).
// Interrupts off.
void mouse_motion_wheel(int32_t rel_x, int32_t rel_y, int32_t wheel, uint8_t buttons);

// True once the PS/2 mouse answered as a wheel mouse (4-byte packets)
bool mouse_has_wheel(void);

void mouse_set_position(uint32_t x, uint32_t y);
Point mouse_get_position(void);

bool mouse_get_button(uint8_t button);

// Mouse event queue

void mouse_enqueue_event(int32_t x, int32_t y, bool left, bool right, bool middle, int32_t wheel);
bool mouse_dequeue_event(MouseEvent* out_event);

// True if an event is queued (a hint: another consumer may take it first)
bool mouse_event_pending(void);

#endif

