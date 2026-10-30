#!/usr/bin/env bash
# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds EXRay_v<version>.exe (Windows x64) with llvm-mingw from Linux.
# Everything is linked statically: the .exe needs no extra DLLs on Windows 10/11.
#
#   LLVM_MINGW=/path/to/llvm-mingw ./build.sh [x86_64|aarch64]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ARCH="${1:-x86_64}"
LLVM_MINGW="${LLVM_MINGW:-$(ls -d "$ROOT"/../tools/llvm-mingw-* 2>/dev/null | head -1)}"
CXX="$LLVM_MINGW/bin/$ARCH-w64-mingw32-clang++"
CC="$LLVM_MINGW/bin/$ARCH-w64-mingw32-clang"
RC="$LLVM_MINGW/bin/$ARCH-w64-mingw32-windres"
OUT="$ROOT/build/$ARCH"
# Version from main_win.cpp (kVersion), e.g. "0.9.4 beta" -> EXRay_v0.9.4_beta.exe
VERSION="$(sed -n 's/.*kVersion\[\] *= *L"\([^"]*\)".*/\1/p' "$ROOT/src/main_win.cpp" | head -1)"
EXE="EXRay_v${VERSION// /_}.exe"
mkdir -p "$OUT"

INC=(-I"$ROOT/src" -I"$ROOT/third_party/tinyexr" -I"$ROOT/third_party/tinyexr/deps/miniz" -I"$ROOT/third_party/stb")
OPT=(-O2 -DNDEBUG -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

"$RC" -I"$ROOT/res" -i "$ROOT/res/app.rc" -o "$OUT/app_res.o"
"$CC"  "${OPT[@]}" "${INC[@]}" -w -c "$ROOT/third_party/tinyexr/deps/miniz/miniz.c" -o "$OUT/miniz.o"
"$CXX" "${OPT[@]}" "${INC[@]}" -std=c++17 -w -c "$ROOT/src/image_io.cpp" -o "$OUT/image_io.o"
OBJS=("$OUT/image_io.o" "$OUT/miniz.o" "$OUT/app_res.o")
for src in tonemap resample thumbnail thumb_pool exr_doc export batch_export hdri_fix fix_window library side_panels settings_win display_win folder_nav toolbar grid_view info_window pano main_win; do
  "$CXX" "${OPT[@]}" "${INC[@]}" -std=c++17 -Wall -Wextra -c "$ROOT/src/$src.cpp" -o "$OUT/$src.o"
  OBJS+=("$OUT/$src.o")
done

"$CXX" -municode -mwindows -static -s "${OBJS[@]}" \
  -lgdi32 -luser32 -lshell32 -lcomdlg32 -lshlwapi -lole32 -luuid \
  -o "$OUT/$EXE"

echo "Built $OUT/$EXE"
