idf_component_register(
    SRCS "main.c" "face_view.c" "face_motion.c" "face_lipsync.c" "face_directive.c" "face_catalogue.c" "audio.c" "voice.c"
         "mem_report.c" "net.c"
         "generated/face_assets.c"
    INCLUDE_DIRS "." "generated"
    REQUIRES lvgl esp_lcd esp_lcd_gc9a01 driver esp_timer
             esp_wifi esp_netif esp_event nvs_flash mqtt esp-tls json spi_flash esp_partition
             esp_websocket_client esp_audio_codec mbedtls
    EMBED_FILES "../tools/lipsync-fixture.pcm"
    EMBED_TXTFILES "certs/AmazonRootCA1.pem" "certs/isrgrootx1.pem")
