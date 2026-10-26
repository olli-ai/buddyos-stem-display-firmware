# ESP-SR 2.1.2, vendored and trimmed to what this device uses: WakeNet on the
# ESP32-S3, nothing else. See VENDORED.md for what was removed and why.
idf_component_register(SRCS "src/model_path.c" "src/esp_sr_debug.c"
                       INCLUDE_DIRS "include/${IDF_TARGET}" "src/include"
                       REQUIRES json spiffs esp_partition
                       PRIV_REQUIRES spi_flash)

set(lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/lib/${IDF_TARGET}")
foreach(l dl_lib c_speech_features esp_audio_processor esp_audio_front_end hufzip wakenet)
    add_prebuilt_library(${l} "${lib_dir}/lib${l}.a" PRIV_REQUIRES ${COMPONENT_NAME})
endforeach()
idf_component_get_property(esp_dsp_lib esp-dsp COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--start-group"
    dl_lib $<TARGET_FILE:${esp_dsp_lib}> c_speech_features esp_audio_front_end
    esp_audio_processor hufzip wakenet
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
