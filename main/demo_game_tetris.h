// main/demo_game_tetris.h - 《俄罗斯方块》(Tetris)
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

void demo_game_tetris_enter(void);
void demo_game_tetris_exit(void);
void demo_game_tetris_key(bsp_btn_t btn, bsp_btn_ev_t ev);
