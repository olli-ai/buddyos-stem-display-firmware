set(srcs "main.c" "gallery.c" "audio.c" "voice.c"
         "st7701.c" "touch.c"
         "mem_report.c" "net.c" "prov.c" "sleep.c")

# The photos are the builder's own, baked by tools/bake_gallery.ps1 and never committed.
set(pack "${CMAKE_CURRENT_LIST_DIR}/../gallery/gallery.bin")
if(NOT EXISTS "${pack}")
    message(FATAL_ERROR "gallery/gallery.bin does not exist. "
                        "Put photos in gallery/ and run tools/bake_gallery.ps1.")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "generated"
    REQUIRES lvgl esp_lcd driver esp_timer
             esp_wifi esp_netif esp_event nvs_flash mqtt esp-tls json
             esp_websocket_client esp_audio_codec mbedtls esp-sr buddy_ble esp_app_format
    EMBED_FILES "${pack}"
    EMBED_TXTFILES "certs/AmazonRootCA1.pem" "certs/isrgrootx1.pem")
