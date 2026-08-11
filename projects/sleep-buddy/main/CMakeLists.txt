idf_component_register(
    SRCS "main.c" "audio_recorder.c" "recorder_ui.c" "wav_writer.c"
         "snore_event_detector.c" "sleep_session.c" "sleep_storage.c"
         "snore_classifier.cpp" "app_settings.c"
         "sleep_engine.c" "wifi_manager.c" "time_zones.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_4_3 esp_codec_dev fatfs nvs_flash esp_timer
             esp_wifi esp_netif lwip esp-tflite-micro
    EMBED_FILES "../local_models/efficientat_mn04_snoring_int8.tflite"
)
