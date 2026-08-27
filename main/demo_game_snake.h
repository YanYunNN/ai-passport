// main/demo_game_snake.h - 《贪吃蛇》(Snake)
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

void demo_game_snake_enter(void);
void demo_game_snake_exit(void);
void demo_game_snake_key(bsp_btn_t btn, bsp_btn_ev_t ev);
