idf_component_register(
    SRCS "main.c" "audio_recorder.c" "recorder_ui.c" "wav_writer.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_4_3 esp_codec_dev fatfs
)
