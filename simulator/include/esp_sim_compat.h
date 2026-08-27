/* simulator/include/esp_sim_compat.h
 * ESP-IDF 外设枚举/句柄的宿主垫片。
 * 仅供 components/bsp/include 下的真实头文件在 PC 上编译通过：
 * bsp_pins.h 是引脚/参数的唯一事实来源，这里只提供其引用的枚举与句柄类型，
 * 不复制任何硬件参数。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* driver/spi_master.h → bsp_pins.h 使用 SPI2_HOST */
typedef enum {
    SPI2_HOST = 2,
} spi_host_device_t;

/* driver/i2c_types.h → bsp_pins.h 使用 I2C_NUM_0 */
typedef enum {
    I2C_NUM_0 = 0,
    I2C_NUM_1 = 1,
} i2c_port_t;

/* driver/i2c_master.h → bsp_i2c.h 使用 i2c_master_bus_handle_t */
typedef void *i2c_master_bus_handle_t;

/* hal/adc_types.h → bsp_pins.h 使用 ADC_UNIT_1 / ADC_CHANNEL_0 */
typedef enum {
    ADC_UNIT_1 = 1,
    ADC_UNIT_2 = 2,
} adc_unit_t;

typedef enum {
    ADC_CHANNEL_0 = 0,
    ADC_CHANNEL_1 = 1,
    ADC_CHANNEL_2 = 2,
    ADC_CHANNEL_3 = 3,
    ADC_CHANNEL_4 = 4,
} adc_channel_t;

/* hal/ledc_types.h → bsp_pins.h 的背光 LEDC 常量 */
typedef enum {
    LEDC_TIMER_0 = 0,
    LEDC_TIMER_1 = 1,
} ledc_timer_t;

typedef enum {
    LEDC_LOW_SPEED_MODE = 0,
    LEDC_HIGH_SPEED_MODE = 1,
} ledc_mode_t;

typedef enum {
    LEDC_CHANNEL_0 = 0,
    LEDC_CHANNEL_1 = 1,
    LEDC_CHANNEL_2 = 2,
    LEDC_CHANNEL_3 = 3,
} ledc_channel_t;

typedef enum {
    LEDC_TIMER_1_BIT = 1,
    LEDC_TIMER_2_BIT = 2,
    LEDC_TIMER_3_BIT = 3,
    LEDC_TIMER_4_BIT = 4,
    LEDC_TIMER_5_BIT = 5,
    LEDC_TIMER_6_BIT = 6,
    LEDC_TIMER_7_BIT = 7,
    LEDC_TIMER_8_BIT = 8,
    LEDC_TIMER_9_BIT = 9,
    LEDC_TIMER_10_BIT = 10,
    LEDC_TIMER_11_BIT = 11,
    LEDC_TIMER_12_BIT = 12,
    LEDC_TIMER_13_BIT = 13,
    LEDC_TIMER_14_BIT = 14,
    LEDC_TIMER_15_BIT = 15,
    LEDC_TIMER_16_BIT = 16,
    LEDC_TIMER_17_BIT = 17,
    LEDC_TIMER_18_BIT = 18,
    LEDC_TIMER_19_BIT = 19,
    LEDC_TIMER_20_BIT = 20,
} ledc_timer_bit_t;

#ifdef __cplusplus
}
#endif
