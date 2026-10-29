# volume_ui.c is not under CONFIG_BUDDY_SCREEN_FACE on purpose: the gallery build
# has the same touch panel and the same speaker, and the drag works there too.
set(srcs "main.c" "board_i2c.c" "status.c" "voice.c" "touch.c"
         "mem_report.c" "net.c" "prov.c" "sleep.c" "setup_ui.c" "volume_ui.c" "clock_ui.c")

# The two screens are exclusive (BUDDY_SCREEN in Kconfig), and so are their
# lip-sync front ends: the face classifier returns a sprite index to draw, the
# gallery one a viseme class a jaw can only approximate.
if(CONFIG_BUDDY_SCREEN_FACE)
    list(APPEND srcs "face_view.c" "face_motion.c" "face_lipsync.c" "face_directive.c"
                     "face_catalogue.c" "face_demo_motion.c")
    # The face is vector art rasterised offline, so its cost is area and the bake
    # belongs to the glass: 720 for the P4's JD9365, 480 for the S3's ST7701S. Both
    # are committed (nothing here needs the front end to build) and exactly one is
    # compiled — they define the same symbols. face_view.c takes face_bg's size *as*
    # the panel's, so the wrong bake does not scale to fit: it draws short of the
    # edges, and the 720 one does not fit the S3's app slot at all.
    #
    # Regenerating them needs tools/bake_face.ts (FACE_SIZE picks the size), which is
    # untracked on purpose — it imports the website's catalogue and cannot compile
    # without that repo. See .gitignore. Building the firmware never needs it.
    #
    # Chosen on IDF_TARGET rather than a CONFIG_: this list is read in the early pass
    # that resolves sources, where no CONFIG_ symbol exists yet. IDF_TARGET does.
    if(IDF_TARGET STREQUAL "esp32p4")
        list(APPEND srcs "generated/face_assets_720.c")
        list(APPEND srcs "face_ppa.c")           # the P4's 2D accelerator
    else()
        list(APPEND srcs "generated/face_assets_480.c")
    endif()
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
    # esp_driver_ppa is the P4's 2D accelerator, and it is required here rather than
    # beside the face's other sources on purpose: a component's CMakeLists is read
    # twice, and in the early pass that resolves REQUIRES no CONFIG_ symbol exists
    # yet. Guarded by CONFIG_BUDDY_SCREEN_FACE it silently resolved to nothing and
    # the build failed on a missing driver/ppa.h while the component sat unbuilt.
    # IDF_TARGET *is* set in that pass, which is why the panel driver below works.
    list(APPEND reqs esp_lcd_jd9365 esp_codec_dev esp_wifi_remote esp_hosted esp_driver_ppa)
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

# The characters a design can name (Sprout, Pip, Leaf), baked from the website's art by
# tools/bake_catalogue.ts and committed as faces/face_catalogue.bin. Written with the
# app, so `write-flash "@flash_args"` puts them on the kit and no one has to know the
# partition's offset. The gallery build never reads them, and a table without a
# `faces` partition has nowhere to put them.
if(CONFIG_BUDDY_SCREEN_FACE AND CONFIG_PARTITION_TABLE_CUSTOM)
    partition_table_get_partition_info(faces_offset "--partition-name faces" "offset")
    if("${faces_offset}")
        # Copied into the build dir so flash_args names it there: a build dir copied to
        # another machine, or out of a container, still flashes on its own.
        idf_build_get_property(build_dir BUILD_DIR)
        configure_file("${CMAKE_CURRENT_LIST_DIR}/../faces/face_catalogue.bin"
                       "${build_dir}/faces/face_catalogue.bin" COPYONLY)
        esptool_py_flash_to_partition(flash "faces" "${build_dir}/faces/face_catalogue.bin")
    endif()
endif()
