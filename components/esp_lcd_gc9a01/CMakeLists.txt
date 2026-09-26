idf_component_register(SRCS "esp_lcd_gc9a01.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "esp_lcd"
                       PRIV_REQUIRES "driver")

# include(package_manager)   # esp-bsp helper; not vendored, and only sets a version string
# cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})   # esp-bsp helper; not vendored, and only sets a version string

# The two lines above normally come from esp-bsp's cmake_utilities, which is not
# vendored here. They only define a version triple that the driver prints in a log
# line at init, so it is supplied directly rather than dragging in another component.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_GC9A01_VER_MAJOR=1
    ESP_LCD_GC9A01_VER_MINOR=2
    ESP_LCD_GC9A01_VER_PATCH=0)
