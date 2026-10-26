idf_component_register(SRCS "esp_lcd_jd9365.c" INCLUDE_DIRS "include" PRIV_REQUIRES "driver" REQUIRES "esp_lcd")

# include(package_manager)   # esp-iot-solution helper; not vendored, and only sets a version string
# cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})

# Those two lines normally come from cmake_utilities, which is not vendored here. They
# only define the version triple the driver prints at init, so it is supplied directly.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_JD9365_VER_MAJOR=2
    ESP_LCD_JD9365_VER_MINOR=0
    ESP_LCD_JD9365_VER_PATCH=2)
