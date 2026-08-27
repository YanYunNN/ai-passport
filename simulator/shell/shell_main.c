/* simulator/shell/shell_main.c —— PC 模拟器【通用外壳】入口。
 *
 * 职责（不含任何固件代码）：
 *   1. 加载可插拔固件模块（simulator/firmware/ 下的 .dll/.so/.dylib）
 *   2. 把服务表 sim_api_t 交给固件，驱动其 start()/frame()/key()/quit()
 *   3. SDL 窗口与事件泵（SDL2.dll 为进程级共享，窗口由固件侧 bsp_display 创建）
 *   4. --autotest 脚本化按键冒烟测试
 *
 * 用法：
 *   sim_shell [--frames=N] [--autotest] [--firmware=<文件或目录>]
 */
#include "sim_api.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 平台加载器封装（Windows: LoadLibrary / 其它: dlopen） ---------------- */
#if defined(_WIN32)
#include <windows.h>

static void *dl_open(const char *path)
{
    return (void *)LoadLibraryA(path);
}
static void *dl_sym(void *handle, const char *name)
{
    return (void *)GetProcAddress((HMODULE)handle, name);
}
static void dl_close(void *handle)
{
    FreeLibrary((HMODULE)handle);
}
#else
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static void *dl_open(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *dl_sym(void *handle, const char *name)
{
    return dlsym(handle, name);
}
static void dl_close(void *handle)
{
    dlclose(handle);
}
#endif

/* ---------------- 可执行文件所在目录（用于定位默认 firmware/） ---------------- */
static bool exe_dir(char *out, size_t size)
{
    if (!out || size == 0) return false;
    out[0] = '\0';

#if defined(_WIN32)
    if (GetModuleFileNameA(NULL, out, (DWORD)size) == 0) return false;
#elif defined(__APPLE__)
    uint32_t len = (uint32_t)size;
    if (_NSGetExecutablePath(out, &len) != 0) return false;
#else
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n <= 0) return false;
    out[n] = '\0';
#endif

    /* 去掉末尾文件名 */
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    char *back = strrchr(out, '\\');
    if (back > slash) slash = back;
#endif
    if (slash) *slash = '\0';
    return out[0] != '\0';
}

static bool file_exists(const char *path)
{
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
#endif
}

static bool dir_exists(const char *path)
{
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* 扫描目录，取第一个固件模块文件（.dll / .so / .dylib） */
static bool scan_firmware_dir(const char *dir, char *out, size_t size)
{
    if (!dir_exists(dir)) return false;

#if defined(_WIN32)
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    snprintf(out, size, "%s\\%s", dir, fd.cFileName);
    FindClose(h);
    return true;
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (ext && (strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0)) {
            snprintf(out, size, "%s/%s", dir, e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
#endif
}

/* 定位固件模块：--firmware 参数 > 环境变量 SIM_FIRMWARE_DIR > exe 旁/上级 firmware/ > CWD/firmware */
static bool locate_firmware(const char *arg_path, char *out, size_t size)
{
    if (arg_path && arg_path[0]) {
        if (file_exists(arg_path)) {
            snprintf(out, size, "%s", arg_path);
            return true;
        }
        return scan_firmware_dir(arg_path, out, size);
    }

    const char *env_dir = getenv("SIM_FIRMWARE_DIR");
    if (env_dir && env_dir[0]) {
        if (scan_firmware_dir(env_dir, out, size)) return true;
        if (file_exists(env_dir)) {
            snprintf(out, size, "%s", env_dir);
            return true;
        }
    }

    char dir[512];
    if (exe_dir(dir, sizeof(dir))) {
        char candidate[600];
        snprintf(candidate, sizeof(candidate), "%s/firmware", dir);
        if (scan_firmware_dir(candidate, out, size)) return true;
        snprintf(candidate, sizeof(candidate), "%s/../firmware", dir);
        if (scan_firmware_dir(candidate, out, size)) return true;
    }

    return scan_firmware_dir("firmware", out, size);
}

/* ---------------- autotest 脚本（注入 SDL 按键事件，模拟真实键盘时序） ---------------- */
typedef struct {
    int frame;
    int sdl_key;
    int down;
} autotest_step_t;

/* 覆盖：菜单导航(CLICK) → 进入图片页 → 长按返回(LONG) → 进入游戏页
 * → 进入合成大西瓜 → 长按退出 → 返回主菜单。脚本不校验结果，进程不崩溃即通过。 */
static const autotest_step_t AUTOTEST[] = {
    {  40, SDLK_DOWN,    1 }, {  42, SDLK_DOWN,    0 },
    {  80, SDLK_RETURN,  1 }, {  82, SDLK_RETURN,  0 },
    { 400, SDLK_RETURN,  1 },
    { 680, SDLK_RETURN,  0 },
    { 720, SDLK_DOWN,    1 }, {  722, SDLK_DOWN,    0 },
    { 760, SDLK_RETURN,  1 }, {  762, SDLK_RETURN,  0 },
    { 800, SDLK_RETURN,  1 }, {  802, SDLK_RETURN,  0 },
    {1000, SDLK_RETURN,  1 },
    {1280, SDLK_RETURN,  0 },
    {1320, SDLK_DOWN,    1 }, { 1322, SDLK_DOWN,    0 },
    {1340, SDLK_DOWN,    1 }, { 1342, SDLK_DOWN,    0 },
    {1360, SDLK_RETURN,  1 }, { 1362, SDLK_RETURN,  0 },
};
#define AUTOTEST_COUNT (sizeof(AUTOTEST) / sizeof(AUTOTEST[0]))

static void print_usage(const char *argv0)
{
    printf("FoloToy AI Passport PC simulator shell\n");
    printf("用法: %s [--frames=N] [--autotest] [--firmware=<文件或目录>]\n", argv0);
    printf("  --frames=N       渲染 N 帧后退出（冒烟测试）\n");
    printf("  --autotest       脚本化注入按键序列（导航/进出页面/长按返回）\n");
    printf("  --firmware=路径  指定固件模块文件或目录（默认: exe 旁/上级的 firmware/ 或 $SIM_FIRMWARE_DIR）\n");
    printf("按键: UP=W/↑  DOWN=S/↓  OK=Enter/Space  ESC/关窗=退出\n");
}

int main(int argc, char **argv)
{
    int frames = -1;
    bool autotest = false;
    const char *firmware_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--frames=", 9) == 0) {
            frames = atoi(argv[i] + 9);
        } else if (strcmp(argv[i], "--autotest") == 0) {
            autotest = true;
            if (frames < 0) frames = 1500;
        } else if (strncmp(argv[i], "--firmware=", 11) == 0) {
            firmware_arg = argv[i] + 11;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("未知参数: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* ---------------- 加载固件模块 ---------------- */
    char firmware_path[700];
    if (!locate_firmware(firmware_arg, firmware_path, sizeof(firmware_path))) {
        fprintf(stderr, "[shell] 未找到固件模块。请先构建 sim_firmware 目标，或用 --firmware= 指定。\n");
        print_usage(argv[0]);
        return 1;
    }

    void *module = dl_open(firmware_path);
    if (!module) {
        fprintf(stderr, "[shell] 加载固件模块失败: %s\n", firmware_path);
        return 1;
    }

    sim_firmware_load_fn load = (sim_firmware_load_fn)dl_sym(module, "sim_firmware_load");
    if (!load) {
        fprintf(stderr, "[shell] 固件模块缺少 sim_firmware_load 入口（ABI 不匹配）: %s\n", firmware_path);
        dl_close(module);
        return 1;
    }
    const sim_firmware_exports_t *fw = load();
    if (!fw || fw->abi_version != SIM_FIRMWARE_ABI_VERSION) {
        fprintf(stderr, "[shell] 固件模块 ABI 版本不匹配（期望 %d）\n", SIM_FIRMWARE_ABI_VERSION);
        dl_close(module);
        return 1;
    }
    printf("[shell] 已加载固件模块: %s (%s)\n", firmware_path, fw->name ? fw->name : "?");

    /* ---------------- 服务表：外壳持有实现，固件经胶水转调 ---------------- */
    sim_api_t api;
    memset(&api, 0, sizeof(api));
    api.version = SIM_API_VERSION;
    api.log_vprintf = esp_log_vprintf;
    api.log_set_vprintf = esp_log_set_vprintf;
    api.esp_err_to_name = esp_err_to_name;
    api.task_create = xTaskCreate;
    api.task_delete = vTaskDelete;
    api.task_delay = vTaskDelay;
    api.task_get_tick_count = xTaskGetTickCount;
    api.queue_create = xQueueCreate;
    api.queue_send = xQueueSend;
    api.queue_receive = xQueueReceive;
    api.queue_delete = vQueueDelete;
    api.nvs_flash_init = nvs_flash_init;
    api.nvs_flash_erase = nvs_flash_erase;
    api.nvs_open = nvs_open;
    api.nvs_get_blob = nvs_get_blob;
    api.nvs_set_blob = nvs_set_blob;
    api.nvs_commit = nvs_commit;
    api.nvs_close = nvs_close;

    /* ---------------- SDL 与固件启动 ---------------- */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[shell] SDL_Init 失败: %s\n", SDL_GetError());
        dl_close(module);
        return 1;
    }

    if (fw->start(&api) != 0) {
        fprintf(stderr, "[shell] 固件启动失败\n");
        dl_close(module);
        return 1;
    }
    printf("[shell] 就绪。按键: UP=W/↑ DOWN=S/↓ OK=Enter/Space, ESC 退出\n");

    /* ---------------- 事件循环 ---------------- */
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
                fw->key((int)event.key.keysym.sym, event.type == SDL_KEYDOWN);
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
                    fw->key(AUTOTEST[i].sdl_key, AUTOTEST[i].down);
                }
            }
        }

        fw->frame(); /* 固件侧：按键连发状态机 + lv_timer_handler */

        if (frames > 0 && ++frame >= frames) {
            quit = true;
        }
        SDL_Delay(5);
    }

    fw->quit();
    printf("[shell] 退出\n");
    dl_close(module);
    SDL_Quit();
    return 0;
}
