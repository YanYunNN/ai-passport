// main/demo.h —— 每个演示页实现的统一接口。
// 新增一个演示页 = 实现这三个函数 + 在 main.c 的 DEMOS[] 里加一行。
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

typedef struct {
    const char *name;
    void (*enter)(void);                          // 建自己的屏并载入
    void (*exit)(void);                           // 删屏、停定时器、释放资源
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  // 收按键(长按确定已被 main 拦截)
    bool (*back)(void);                           // 可选：二级返回拦截（返回 true 表示内部已消费返回上一级）
} demo_entry_t;

// 各演示页(定义在各自的 .c 里)
void demo_display_enter(void); void demo_display_exit(void);
void demo_display_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_button_enter(void);  void demo_button_exit(void);
void demo_button_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_audio_enter(void);   void demo_audio_exit(void);
void demo_audio_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_battery_enter(void); void demo_battery_exit(void);
void demo_battery_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_reader_enter(void);  void demo_reader_exit(void);
void demo_reader_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_image_enter(void);   void demo_image_exit(void);
void demo_image_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_settings_enter(void); void demo_settings_exit(void);
void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool demo_settings_back(void);

void demo_kiro_passport_enter(void); void demo_kiro_passport_exit(void);
void demo_kiro_passport_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_chat_enter(void);   void demo_chat_exit(void);
void demo_chat_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool demo_chat_back(void);

void demo_anim_enter(void);    void demo_anim_exit(void);
void demo_anim_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_builtin_enter(void); void demo_builtin_exit(void);
void demo_builtin_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool demo_builtin_back(void);
void demo_builtin_set_on_exit(void (*on_exit)(void));

void demo_games_enter(void);   void demo_games_exit(void);
void demo_games_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool demo_games_back(void);
void demo_games_set_on_exit(void (*on_exit)(void));


