#include "floor_ctrl.h"

static floor_state_t s_floor_state = FLOOR_FREE;

void floor_ctrl_init(void)
{
    s_floor_state = FLOOR_FREE;
}

floor_state_t floor_ctrl_get(void)
{
    return s_floor_state;
}

void floor_ctrl_set(floor_state_t state)
{
    s_floor_state = state;
}

bool floor_ctrl_is_granted(void)
{
    return s_floor_state == FLOOR_GRANTED;
}
