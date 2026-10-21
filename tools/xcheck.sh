#!/usr/bin/env bash
# Build and run the conformance check on the host. See tools/xcheck.c.
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
STUB
gcc -std=gnu11 -O1 -Wall -I /tmp/xcheck -I main -I main/generated \
    tools/xcheck.c main/face_motion.c main/face_lipsync.c main/generated/face_assets.c \
    -lm -o /tmp/xcheck/xcheck
exec /tmp/xcheck/xcheck main/generated tools/lipsync-fixture.pcm
