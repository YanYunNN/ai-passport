# simulator/CMakeLists.txt
# PC 模拟器（通用外壳 + 可插拔固件模块）。
#
# 目标平台：Windows (MinGW) / macOS / Linux。需要 CMake + SDL2。
#
#   cmake -S simulator -B simulator/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   cmake --build simulator/build
#
# 产出：
#   build/sim_shell.exe          通用外壳（SDL 事件泵 + 固件加载器，不含固件代码）
#   firmware/sim_firmware.dll    固件模块（main/ 源码 + 宿主适配，可插拔加载）
#
# 运行：sim_shell 默认从 exe 旁/上级的 firmware/ 目录加载固件模块。
# SDL2 查找：优先 SDL2Config.cmake；找不到时可用 CMAKE_PREFIX_PATH 指向 SDL2 安装目录。
cmake_minimum_required(VERSION 3.16)
project(ai_passport_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# SDL2（跨平台：Win/macOS/Linux）
# ---------------------------------------------------------------------------
find_package(SDL2 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# LVGL 9.5.0（与 components/bsp/idf_component.yml 的 ^9.5.0 对齐；只链接进固件模块）
# ---------------------------------------------------------------------------
include(FetchContent)
set(LVGL_VERSION 9.5.0)
FetchContent_Declare(lvgl
    URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.5.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# LVGL 桌面构建配置（在 add_subdirectory 之前设置）
set(LV_BUILD_CONF_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH
    "simulator/lv_conf.h 所在目录" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_GetProperties(lvgl)
if(NOT lvgl_POPULATED)
    FetchContent_MakeAvailable(lvgl)
endif()

# lv_conf.h 启用了 LV_USE_SDL，SDL 驱动源码在 lvgl 库内编译，需要 SDL2 头文件与链接
# （LVGL 桌面 CMake 不会自己找 SDL2）
target_link_libraries(lvgl PUBLIC SDL2::SDL2)

# 固件/外壳共用的编译兼容定义（MinGW 宿主）
function(sim_apply_host_defines target)
    target_compile_definitions(${target} PRIVATE
        _POSIX_THREAD_SAFE_FUNCTIONS
        __USE_MINGW_ANSI_STDIO=1
    )
endfunction()

# ---------------------------------------------------------------------------
# 固件模块（可插拔）：main/ 源码原样编译 + 模块侧宿主适配
# ---------------------------------------------------------------------------
set(FIRMWARE_MAIN_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_pixel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_pixel_math.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_system.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_font_noto_sc_14.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_font_noto_sc_20.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/app_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/debug_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_reader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_image.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_button.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_battery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_builtin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_games.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_game_suika.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_game_bubble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_game_tetris.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_game_snake.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_game_flappy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/demo_kiro_passport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/game_audio.c
)

set(FIRMWARE_MODULE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/module/module_entry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/bsp_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/bsp_button.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/bsp_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/bsp_battery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/bsp_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/stubs_system.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/stubs_wifi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/module/stubs_kiro.c
)

add_library(sim_firmware MODULE ${FIRMWARE_MAIN_SRC} ${FIRMWARE_MODULE_SRC})
sim_apply_host_defines(sim_firmware)
target_include_directories(sim_firmware PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # ESP-IDF 垫片头文件 + sim_api.h
    ${CMAKE_CURRENT_SOURCE_DIR}           # lv_conf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/module
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bsp/include
)
target_link_libraries(sim_firmware PRIVATE lvgl SDL2::SDL2)
if(WIN32)
    # MinGW-w64 (winpthreads)；MSVC 未支持（main/ 使用了 __attribute__((packed))）
    target_link_libraries(sim_firmware PRIVATE pthread)
else()
    target_link_libraries(sim_firmware PRIVATE pthread m)
endif()

# 固件模块输出到 simulator/firmware/（外壳从这里扫描加载）
set_target_properties(sim_firmware PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/firmware
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/firmware
)

# ---------------------------------------------------------------------------
# 通用外壳：SDL 事件泵 + 固件加载器 + 服务表实现（不含固件代码）
# ---------------------------------------------------------------------------
add_executable(sim_shell
    ${CMAKE_CURRENT_SOURCE_DIR}/shell/shell_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/shell/shim_esp_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/shell/shim_freertos.c
    ${CMAKE_CURRENT_SOURCE_DIR}/shell/shim_nvs.c
)
sim_apply_host_defines(sim_shell)
target_include_directories(sim_shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shell
)
# SDL2main：Windows 下提供 WinMain → SDL_main（main() 被 SDL_main.h 重命名），
# 同时带来 SDL2 的 -mwindows 链接选项。注意顺序：SDL2main 必须先于 SDL2，
# 静态库单遍解析，否则其内部对 SDL_SetMainReady 的引用无法解析。
target_link_libraries(sim_shell PRIVATE SDL2::SDL2main SDL2::SDL2)
if(WIN32)
    target_link_libraries(sim_shell PRIVATE pthread)
else()
    target_link_libraries(sim_shell PRIVATE pthread m)
endif()
