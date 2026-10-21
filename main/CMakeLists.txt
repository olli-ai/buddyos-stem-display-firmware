set(srcs "main.c" "board_i2c.c" "status.c" "voice.c" "touch.c"
         "mem_report.c" "net.c" "prov.c" "sleep.c" "setup_ui.c")

# The two screens are exclusive (BUDDY_SCREEN in Kconfig), and so are their
# lip-sync front ends: the face classifier returns a sprite index to draw, the
# gallery one a viseme class a jaw can only approximate.
if(CONFIG_BUDDY_SCREEN_FACE)
    list(APPEND srcs "face_view.c" "face_motion.c" "face_lipsync.c" "face_directive.c"
                     "face_catalogue.c" "generated/face_assets.c")
else()
    list(APPEND srcs "gallery.c" "lipsync.c" "generated/visemes.c")
endif()
set(reqs lvgl esp_lcd driver esp_timer
         esp_wifi esp_netif esp_event nvs_flash mqtt esp-tls json
         esp_websocket_client esp_audio_codec mbedtls esp-sr buddy_ble esp_app_format)
# face_catalogue reads the artwork partition (ADR 0009).
if(CONFIG_BUDDY_SCREEN_FACE)
    list(APPEND reqs spi_flash esp_partition)
endif()

if(IDF_TARGET STREQUAL "esp32p4")
    # Waveshare ESP32-P4-WIFI6-Touch-LCD-4C: 720x720 MIPI-DSI, ES8311 + ES7210, and Wi-Fi
    # and BLE from the ESP32-C6 beside it, over SDIO.
    list(APPEND srcs "lcd_p4.c" "audio_p4.c")
    list(APPEND reqs esp_lcd_jd9365 esp_codec_dev esp_wifi_remote esp_hosted)
    set(pack_name "gallery-720.bin")
else()
    # The ESP32-S3 bench board: 480x480 round ST7701S, INMP441 + MAX98357A.
    list(APPEND srcs "st7701.c" "audio.c")
    set(pack_name "gallery.bin")
endif()

# The photos are the builder's own, baked by tools/bake_gallery.ps1 and never committed.
# Only the gallery build needs photos; the face carries its art in face_assets.c.
set(pack "${CMAKE_CURRENT_LIST_DIR}/../gallery/${pack_name}")
set(embed "")
if(CONFIG_BUDDY_SCREEN_GALLERY)
    if(NOT EXISTS "${pack}")
        message(FATAL_ERROR "gallery/${pack_name} does not exist. Put photos in gallery/ and run "
                            "tools/bake_gallery.ps1 (with -Side 720 for the ESP32-P4 board).")
    endif()
    set(embed "${pack}")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "generated"
    REQUIRES ${reqs}
    EMBED_FILES ${embed}
    EMBED_TXTFILES "certs/AmazonRootCA1.pem" "certs/isrgrootx1.pem")
