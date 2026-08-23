idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "ui_font_noto_sc_14.c"
         "ui_font_noto_sc_20.c"
         "ui_system.c"
         "ui_status.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_reader.c"
         "demo_settings.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
)
