# ESP-SR 2.4.7, vendored and trimmed to what this device uses: WakeNet on the
# ESP32-S3, and on the ESP32-P4 the AFE (echo cancellation) in front of it.
# See VENDORED.md for what was removed and why.
#
# lib/esp32p4 is upstream's build for P4 silicon v3 and later, which is what the
# Waveshare board carries and what sdkconfig.defaults.esp32p4 selects. Upstream also
# ships an esp32p4_less_v3 set for older chips; it is not vendored, so refuse a build
# that would need it rather than link libraries the chip faults on.
if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    message(FATAL_ERROR "esp-sr: only the P4 v3 libraries are vendored; see VENDORED.md")
endif()

# cJSON from IDF's own `json` component, which is the same 1.7.19 that upstream pulls
# from the registry as espressif/cjson: two copies would be two cJSON_Parse.
idf_component_register(SRCS "src/model_path.c" "src/esp_sr_debug.c"
                       INCLUDE_DIRS "include/${IDF_TARGET}" "src/include"
                       REQUIRES json spiffs esp_partition esp-dsp dl_fft
                       PRIV_REQUIRES spi_flash)

set(lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/lib/${IDF_TARGET}")
# nsnet and vadnet: the AFE links against both even with noise suppression and VAD
# left off. On the S3, where only WakeNet runs, nothing pulls them in.
set(libs dl_lib c_speech_features esp_audio_processor esp_audio_front_end hufzip
         nsnet vadnet wakenet)
set(lib_files "")
foreach(l ${libs})
    add_prebuilt_library(${l} "${lib_dir}/lib${l}.a" PRIV_REQUIRES ${COMPONENT_NAME})
    list(APPEND lib_files "${lib_dir}/lib${l}.a")
endforeach()
# By path, not by target name: CMake drops a library target it has already placed on
# the link line, which emptied this group of everything but esp-dsp — and on the P4
# esp_audio_processor's noise suppression needs the FFT in c_speech_features, which
# sits *before* it. This component's own library is in the group too: WakeNet calls
# back into src/esp_sr_debug.c. dl_fft and cJSON are called from inside the
# prebuilt libraries, so they belong in the group for the same reason.
idf_component_get_property(esp_dsp_lib esp-dsp COMPONENT_LIB)
idf_component_get_property(dl_fft_lib dl_fft COMPONENT_LIB)
idf_component_get_property(json_lib json COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--start-group"
    $<TARGET_FILE:${COMPONENT_LIB}> $<TARGET_FILE:${esp_dsp_lib}>
    $<TARGET_FILE:${dl_fft_lib}> $<TARGET_FILE:${json_lib}> ${lib_files}
    "-Wl,--end-group")

# The wake-word model lives in its own `model` partition as srmodels.bin, packed
# at build time from whichever CONFIG_SR_WN_* is selected, and flashed with the app.
if(CONFIG_PARTITION_TABLE_CUSTOM)
    partition_table_get_partition_info(size "--partition-name model" "size")
    partition_table_get_partition_info(offset "--partition-name model" "offset")
    if("${size}" AND "${offset}")
        idf_build_get_property(build_dir BUILD_DIR)
        idf_build_get_property(python PYTHON)
        set(image_file ${build_dir}/srmodels/srmodels.bin)
        add_custom_command(
            OUTPUT ${image_file}
            COMMENT "Packing the wake-word model"
            COMMAND ${python} ${COMPONENT_DIR}/model/movemodel.py -d1 ${SDKCONFIG} -d2 ${COMPONENT_DIR} -d3 ${build_dir}
            DEPENDS ${SDKCONFIG}
            VERBATIM)
        add_custom_target(srmodels_bin ALL DEPENDS ${image_file})
        add_dependencies(flash srmodels_bin)
        esptool_py_flash_to_partition(flash "model" "${image_file}")
    endif()
endif()
