#pragma once
#include <stdbool.h>

typedef enum {
    FLOOR_FREE    = 0,
    FLOOR_GRANTED = 1,
    FLOOR_BUSY    = 2,
} floor_state_t;

void         floor_ctrl_init(void);
floor_state_t floor_ctrl_get(void);
void         floor_ctrl_set(floor_state_t state);
bool         floor_ctrl_is_granted(void);
