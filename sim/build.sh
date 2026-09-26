#!/usr/bin/env bash
# Build and run the headless simulator. Intended to run inside a container that has
# gcc, because this machine's Xcode toolchain is broken:
#
#   docker run --rm -p 8080:8080 -v "$PWD":/fw -w /fw espressif/idf:v5.2.2 \
#       bash sim/build.sh --serve
#
# Then open http://localhost:8080/ . Without --serve it dumps PPM frames instead.
#
# LVGL is cloned on first run and kept in sim/lvgl (gitignored).
set -euo pipefail
cd "$(dirname "$0")/.."
MODE=${1:-sim/frames}

if [ ! -d sim/lvgl ]; then
  git clone --depth 1 --branch release/v9.2 https://github.com/lvgl/lvgl.git sim/lvgl
fi

# lv_conf.h from LVGL's own template, so the simulator tracks whatever the version
# in sim/lvgl expects rather than a copy that rots. Colour depth matches the panel.
if [ ! -f sim/lv_conf.h ]; then
  sed -e '0,/#if 0/s//#if 1/' sim/lvgl/lv_conf_template.h > sim/lv_conf.h
  sed -i 's/^#define LV_COLOR_DEPTH .*/#define LV_COLOR_DEPTH 16/' sim/lv_conf.h
  sed -i 's/^#define LV_USE_LOG .*/#define LV_USE_LOG 1/' sim/lv_conf.h
  # LVGL's default 64 KB heap is left alone ON PURPOSE. An earlier renderer put a
  # transform on the face container, which makes LVGL draw it through a 240x240
  # layer — 115 KB at RGB565 — and with 64 KB the allocation fails and the refresh
  # spins forever. That was found here rather than on hardware. The renderer now
  # composes the head per sprite, so if this still draws at the default heap, the
  # device can afford it too.
fi

[ "$MODE" = "--serve" ] || mkdir -p "$MODE"

# LVGL is ~600 files; building it once into a static library turns a rebuild of our
# own three files from eight minutes into seconds. Rebuilt when lv_conf.h changes.
if [ ! -f sim/liblvgl.a ] || [ sim/lv_conf.h -nt sim/liblvgl.a ]; then
  echo "building liblvgl.a (once)..."
  rm -rf sim/objs && mkdir -p sim/objs
  mapfile -t SRC < <(find sim/lvgl/src -name '*.c')
  printf '%s\n' "${SRC[@]}" | xargs -P "$(nproc)" -I{} sh -c \
    'gcc -std=gnu11 -O1 -w -DLV_CONF_INCLUDE_SIMPLE -I sim -I sim/lvgl -c "$1" -o "sim/objs/$(echo "$1" | tr / _).o"' _ {}
  ar rcs sim/liblvgl.a sim/objs/*.o
fi

gcc -std=gnu11 -O1 -w \
    -DLV_CONF_INCLUDE_SIMPLE \
    -I sim -I sim/lvgl -I main -I main/generated \
    sim/sim_main.c sim/sim_server.c main/face_view.c main/face_motion.c main/generated/face_assets.c \
    sim/liblvgl.a -lm -o sim/facesim
exec ./sim/facesim "$MODE" "${2:-}"
