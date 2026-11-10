#!/usr/bin/env bash
# Build and run the conformance check on the host. See tools/xcheck.c.
#
#   bash tools/xcheck.sh
#
# It uses $CC, or cc. On this Mac `cc` is the xcode-select wrapper, which is broken
# (it cannot load libxcodebuildLoader.dylib) while the compiler behind it is fine, so
# the SDK is passed explicitly and Xcode's clang is called by its own path:
#
#   CC=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang \
#   SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk \
#   bash tools/xcheck.sh
#
# A container works too and needs neither, but costs gigabytes of Docker disk for
# arithmetic a host compiler does in a second:
#
#   docker run --rm -v "$PWD":/fw -w /fw espressif/idf:v5.2.2 bash tools/xcheck.sh
#
# It needs no LVGL and no panel — only the arithmetic — so face_assets.c is compiled
# against a stub of the one LVGL type it mentions rather than the real library.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p /tmp/xcheck
cat > /tmp/xcheck/lvgl.h <<'STUB'
/* Just enough LVGL for the generated image descriptors to compile on the host. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t magic: 8, cf: 8, flags: 16; uint32_t w: 16, h: 16; uint32_t stride: 16, reserved_2: 16; } lv_image_header_t;
typedef struct { lv_image_header_t header; uint32_t data_size; const uint8_t *data; const void *reserved; } lv_image_dsc_t;
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 0x12
#define LV_COLOR_FORMAT_ARGB8888 0x10
#define LV_COLOR_FORMAT_RGB565A8 0x14
#define LV_COLOR_FORMAT_A8 0x0E          /* the accessory masks */
STUB
# ...and of esp_log.h, which face_motion.c gained when a design's own motion could
# arrive at runtime and be worth reporting. The check wants the arithmetic, not the
# log line, so the macros evaluate to nothing.
cat > /tmp/xcheck/esp_log.h <<'STUB'
/* Host stand-in: the conformance check is arithmetic, and says nothing. */
#pragma once
#define ESP_LOGI(tag, ...) do { } while (0)
#define ESP_LOGW(tag, ...) do { } while (0)
#define ESP_LOGE(tag, ...) do { } while (0)
STUB
# The 240 bake, not the 720: this check is arithmetic — motion sampling and the
# lip-sync classifier — and the sprites are here only so the thing links. The small
# bake is the one the S3 kits ship, and compiling the 720 instead costs 26 MB of
# source for an identical answer.
#
# It was `face_assets.c` until that file was split per panel (72b49b7), and this line
# was not followed, so the check silently stopped building: gcc failed, `set -e` took
# the script down, and the only sign was an exit code nobody was reading. Caught
# 2026-11-03. If the bakes are ever renamed again, this is the second place to look.
"${CC:-cc}" ${SYSROOT:+-isysroot "$SYSROOT"} -std=gnu11 -O1 -Wall -I /tmp/xcheck -I main -I main/generated \
    tools/xcheck.c main/face_motion.c main/face_lipsync.c main/generated/face_assets_240.c \
    -lm -o /tmp/xcheck/xcheck
exec /tmp/xcheck/xcheck main/generated tools/lipsync-fixture.pcm
