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
# The 240 bake, not the 720: this check is arithmetic — motion sampling and the
# lip-sync classifier — and the sprites are here only so the thing links. The small
# bake is the one the S3 kits ship, and compiling the 720 instead costs 26 MB of
# source for an identical answer.
#
# It was `face_assets.c` until that file was split per panel (72b49b7), and this line
# was not followed, so the check silently stopped building: gcc failed, `set -e` took
# the script down, and the only sign was an exit code nobody was reading. Caught
# 2026-11-03. If the bakes are ever renamed again, this is the second place to look.
gcc -std=gnu11 -O1 -Wall -I /tmp/xcheck -I main -I main/generated \
    tools/xcheck.c main/face_motion.c main/face_lipsync.c main/generated/face_assets_240.c \
    -lm -o /tmp/xcheck/xcheck
exec /tmp/xcheck/xcheck main/generated tools/lipsync-fixture.pcm
