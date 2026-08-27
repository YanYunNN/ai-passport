// main/demo_game_flappy.h - 《像素小鸟》(Flappy Bird)
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

void demo_game_flappy_enter(void);
void demo_game_flappy_exit(void);
void demo_game_flappy_key(bsp_btn_t btn, bsp_btn_ev_t ev);
