cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only what main asks for. components/ holds the vendored dependencies of BOTH boards —
# esp_hosted, esp_wifi_remote, the JD9365 driver and esp_codec_dev are the P4's alone —
# and a component that is merely present is otherwise built, and some of them break an
# S3 build just by being compiled. main/CMakeLists.txt names each one per target.
set(COMPONENTS main)
project(buddyos_stem_display)
