/* simulator/src/main.c —— PC 模拟器宿主入口。
 *
 * 流程：
 *   1. SDL + LVGL 初始化
 *   2. 直接调用固件主程序 app_main()（main/ 源码原样编译），
 *      它完成 BSP/LVGL/设置/菜单的全部初始化
 *   3. 宿主事件循环：SDL 事件 → 按键合成 → lv_timer_handler()
 *
 * 用法：
 *   ai_passport_sim [--frames=N]   运行 N 帧后自动退出（冒烟测试）
 *   ai_passport_sim --help
 */
#include "sim_internal.h"

#include "lvgl.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* main/main.c 的固件入口 */
void app_main(void);

/* --autotest 脚本：在第 N 帧注入按键（模拟真实键盘时序）。
 * 覆盖：菜单导航(CLICK) → 进入图片页 → 长按返回(LONG) → 进入游戏页
 * → 长按退出（触发 game_audio 任务/队列的创建与删除）。
 * 脚本只负责注入按键，不校验结果；进程不崩溃即视为通过。 */
typedef struct {
    int frame;
    int sdl_key;
    int down;
} autotest_step_t;

static const autotest_step_t AUTOTEST[] = {
    {  40, SDLK_DOWN,    1 }, {  42, SDLK_DOWN,    0 },  /* 主菜单 阅读→图片 */
    {  80, SDLK_RETURN,  1 }, {  82, SDLK_RETURN,  0 },  /* 进入图片页 */
    { 400, SDLK_RETURN,  1 },                             /* 按住 */
    { 680, SDLK_RETURN,  0 },                             /* 长按返回主菜单 */
    { 720, SDLK_DOWN,    1 }, {  722, SDLK_DOWN,    0 },  /* 主菜单 图片→游戏 */
    { 760, SDLK_RETURN,  1 }, {  762, SDLK_RETURN,  0 },  /* 进入游戏子菜单 */
    { 800, SDLK_RETURN,  1 }, {  802, SDLK_RETURN,  0 },  /* 进入 合成大西瓜 */
    {1000, SDLK_RETURN,  1 },                             /* 按住 */
    {1280, SDLK_RETURN,  0 },                             /* 长按退出游戏回子菜单 */
    {1320, SDLK_DOWN,    1 }, { 1322, SDLK_DOWN,    0 },  /* 子菜单 → 泡泡龙 */
    {1340, SDLK_DOWN,    1 }, { 1342, SDLK_DOWN,    0 },  /* 子菜单 → 返回 */
    {1360, SDLK_RETURN,  1 }, { 1362, SDLK_RETURN,  0 },  /* 返回 → 主菜单 */
};
#define AUTOTEST_COUNT (sizeof(AUTOTEST) / sizeof(AUTOTEST[0]))

static void print_usage(const char *argv0)
{
    printf("FoloToy AI Passport PC simulator (P0)\n");
    printf("用法: %s [--frames=N] [--autotest]\n", argv0);
    printf("  --frames=N  渲染 N 帧后退出（冒烟测试）\n");
    printf("  --autotest  脚本化注入按键序列（导航/进出页面/长按返回）\n");
    printf("按键: UP=W/↑  DOWN=S/↓  OK=Enter/Space  ESC/关窗=退出\n");
}

int main(int argc, char **argv)
{
    int frames = -1;
    bool autotest = false;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--frames=", 9) == 0) {
            frames = atoi(argv[i] + 9);
        } else if (strcmp(argv[i], "--autotest") == 0) {
            autotest = true;
            if (frames < 0) frames = 1500;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("未知参数: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        return 1;
    }

    /* 固件主程序：显示初始化、设置、按键、菜单全部复用 main/ 源码 */
    app_main();

    printf("[sim] 就绪。按键: UP=W/↑ DOWN=S/↓ OK=Enter/Space, ESC 退出\n");

    bool quit = false;
    int frame = 0;

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                if (event.key.repeat) continue; /* 忽略键盘硬件重复 */
                if (event.type == SDL_KEYDOWN &&
                    (event.key.keysym.sym == SDLK_ESCAPE)) {
                    quit = true;
                    break;
                }
                bsp_button_sim_key(event.key.keysym.sym,
                                   event.type == SDL_KEYDOWN);
            }
        }

        /* 自动测试：按帧号注入脚本化按键 */
        if (autotest) {
            for (size_t i = 0; i < AUTOTEST_COUNT; i++) {
                if (AUTOTEST[i].frame == frame) {
                    printf("[autotest] frame %d: %s %s\n", frame,
                           AUTOTEST[i].down ? "press" : "release",
                           AUTOTEST[i].sdl_key == SDLK_DOWN ? "DOWN" :
                           AUTOTEST[i].sdl_key == SDLK_RETURN ? "OK" : "?");
                    bsp_button_sim_key(AUTOTEST[i].sdl_key, AUTOTEST[i].down);
                }
            }
        }

        bsp_button_sim_poll();      /* 长按 / 连发状态机 */
        lv_timer_handler();         /* LVGL 渲染与定时器 */

        if (frames > 0 && ++frame >= frames) {
            quit = true;
        }
        SDL_Delay(5);
    }

    printf("[sim] 退出\n");
    SDL_Quit();
    return 0;
}
