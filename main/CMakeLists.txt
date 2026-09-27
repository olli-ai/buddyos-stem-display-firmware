idf_component_register(
    SRCS "main.c" "face_view.c" "face_motion.c" "face_lipsync.c" "mem_report.c" "net.c"
         "generated/face_assets.c"
    INCLUDE_DIRS "." "generated"
    REQUIRES lvgl esp_lcd esp_lcd_gc9a01 driver esp_timer
             esp_wifi esp_netif esp_event nvs_flash mqtt esp-tls
    EMBED_FILES "../tools/lipsync-fixture.pcm")
