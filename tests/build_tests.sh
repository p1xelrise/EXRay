#!/usr/bin/env bash
# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the native (Linux) test tools into build/linux/ with g++.
# The shared, platform-independent code goes into one static library; each
# test links against it.
#
#   tests/build_tests.sh            # then e.g. build/linux/fix_test
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/linux"
mkdir -p "$OUT"
CXXFLAGS=(-std=c++17 -O2 -pthread -I"$ROOT/src" -I"$ROOT/third_party/tinyexr"
          -I"$ROOT/third_party/tinyexr/deps/miniz" -I"$ROOT/third_party/stb")

OBJS=()
gcc -O2 -w -c "$ROOT/third_party/tinyexr/deps/miniz/miniz.c" -o "$OUT/miniz.o"
OBJS+=("$OUT/miniz.o")
for m in image_io exr_doc export batch_export hdri_fix tonemap resample thumbnail thumb_pool pano; do
  g++ "${CXXFLAGS[@]}" -w -c "$ROOT/src/$m.cpp" -o "$OUT/$m.o"
  OBJS+=("$OUT/$m.o")
done
g++ "${CXXFLAGS[@]}" -w -c "$ROOT/tests/stbw.cpp" -o "$OUT/stbw.o"   # only pulled in by tests that need it
OBJS+=("$OUT/stbw.o")
rm -f "$OUT/libexray.a"
ar rcs "$OUT/libexray.a" "${OBJS[@]}"

for t in fix_test analyze_test radiance_test ground_truth_test light_test decode_test export_test \
         layers_test crypto_test batch_test pano_test phase2_test thumb_test make_big make_crypto make_layered; do
  g++ "${CXXFLAGS[@]}" -include algorithm "$ROOT/tests/$t.cpp" "$OUT/libexray.a" -o "$OUT/$t"
  echo "built build/linux/$t"
done
