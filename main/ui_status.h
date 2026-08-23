#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_STATUS_TIME_HH_MM,
    UI_STATUS_TIME_HH_MM_SS,
} ui_status_time_format_t;

void ui_status_init(void);
void ui_status_set_visible(bool visible);

/* 运行期间有效的 24 小时制时钟设置。 */
void ui_status_set_time(uint8_t hour, uint8_t minute, uint8_t second);
void ui_status_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second);
void ui_status_set_time_format(ui_status_time_format_t format);
ui_status_time_format_t ui_status_get_time_format(void);

/* 仅在外部充电器/VBUS 检测硬件提供可靠状态时调用。 */
void ui_status_set_charging(bool charging);
