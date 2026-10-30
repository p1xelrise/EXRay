#!/usr/bin/env bash
# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs build/x86_64/EXRay_v<version>.exe with the text files into release/dist/<name>.zip (+ SHA-256).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/.*kVersion\[\] *= *L"\([^"]*\)".*/\1/p' "$ROOT/src/main_win.cpp" | head -1)"
N="EXRay_v${VERSION// /_}"
D="$ROOT/release/dist/$N"
rm -rf "$ROOT/release/dist" && mkdir -p "$D"
cp "$ROOT/build/x86_64/$N.exe" "$ROOT/release/LICENSE.txt" "$ROOT/release/THIRD-PARTY-NOTICES.txt" "$ROOT/release/VERSION-HISTORY.txt" "$D/"
python3 - "$ROOT" "$VERSION" "$N" <<'PY'
import sys, os, zipfile
root, ver, n = sys.argv[1:]
t = open(os.path.join(root, 'release/README-template.txt'), encoding='utf-8').read()
t = t.replace('@EXE@             the app', (n + '.exe').ljust(28) + 'the app').replace('@VER@', ver).replace('@EXE@', n + '.exe')
d = os.path.join(root, 'release/dist', n)
open(os.path.join(d, 'README.txt'), 'w', encoding='utf-8', newline='\r\n').write(t)
with zipfile.ZipFile(os.path.join(root, 'release/dist', n + '.zip'), 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for f in sorted(os.listdir(d)):
        z.write(os.path.join(d, f), n + '/' + f)
PY
cd "$ROOT/release/dist" && sha256sum "$N.zip" "$N/$N.exe" | sed "s#$N/##" > "$N.sha256.txt"
echo "Packed release/dist/$N.zip"
