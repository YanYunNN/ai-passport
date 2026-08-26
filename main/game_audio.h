// main/game_audio.h - 游戏音效合成引擎
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    GAME_SFX_NONE = 0,
    GAME_SFX_MOVE,          // 移动/瞄准滴答声 (轻快短促)
    GAME_SFX_DROP,          // 投掷/下落音
    GAME_SFX_SHOOT,         // 泡泡发射 (咻~)
    GAME_SFX_POP,           // 泡泡爆破/轻脆啵声
    GAME_SFX_MERGE_SMALL,   // 小水果合成 "Duang~"
    GAME_SFX_MERGE_MED,     // 中水果合成和弦
    GAME_SFX_MERGE_BIG,     // 大西瓜合成/高分欢呼
    GAME_SFX_COMBO,         // 连击连爆升调
    GAME_SFX_OVER,          // 游戏结束音
    GAME_SFX_WIN,           // 胜利通关欢庆
} game_sfx_t;

void game_audio_init(void);
void game_audio_deinit(void);
void game_audio_play(game_sfx_t sfx);
