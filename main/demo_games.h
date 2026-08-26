// main/demo_games.h - 游戏板块子菜单
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

void demo_games_enter(void);
void demo_games_exit(void);
void demo_games_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool demo_games_back(void);
void demo_games_set_on_exit(void (*on_exit)(void));
