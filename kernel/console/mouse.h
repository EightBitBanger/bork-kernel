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
    uint64_t timestamp;
} MouseEvent;

void mouse_initiate(void);

void mouse_set_cursor_speed(int32_t horz, int32_t vert);
void mouse_set_cursor_acceleration(int32_t acceleration);

void mouse_event_handler(void);

void mouse_set_position(uint32_t x, uint32_t y);
Point mouse_get_position(void);

bool mouse_get_button(uint8_t button);

// Mouse event queue

void mouse_enqueue_event(int32_t x, int32_t y, bool left, bool right, bool middle);
bool mouse_dequeue_event(MouseEvent* out_event);

#endif
