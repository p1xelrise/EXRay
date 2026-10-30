# EXRay — development notes

The user-facing overview is in the [README](../README.md). These notes cover the
code layout, building, the tests and the history of how EXRay was built.


HDR & EXR viewer for 3D artists (called "HDR & EXR file preview" before v0.9.2).

A fast, portable viewer for HDR images on Windows 11: Radiance `.hdr` and
OpenEXR `.exr`, including multi-layer and multi-part renders. One `.exe`,
nothing to install, no extra DLLs — copy it to any Windows 11 PC and run it.

Browse a whole folder of HDRIs as thumbnails, open any image to inspect it with
exposure and tonemap controls, look inside multi-pass renders (depth, normals,
motion vectors, Cryptomatte), and export what you need as PNG or JPEG.

## Main features

- Thumbnail grid of a folder, made in the background, adjustable size (50–200%).
- Image view: zoom, pan, Fit / 100%, arrow-key browsing through the folder.
- 360° view of equirectangular (2:1) HDRIs: drag to look around, wheel for field of view.
- Exposure −10 … +10 EV, tonemap Clip / Reinhard / ACES.
- EXR layers, multi-part and multi-view files; readable depth, normal and
  vector views; pixel readout of the real stored values.
- Cryptomatte: ID colours, object/material names under the mouse, click to
  isolate a mask.
- Export one image or a whole grid selection; look Clip (default), ACES or as
  shown on screen; optionally every layer of multi-layer EXRs.
- Fix HDRI (H): rebuilds a clipped sun, repairs NaN / infinite / negative
  values and hot pixels, warns about "HDRs" that are really ordinary pictures;
  B shows before / after, Ctrl+S saves a copy as EXR (32-bit or half) or HDR.
- Handles 16K HDRIs; settings kept in `EXRay.ini` next to the .exe.
- Help > Controls and shortcuts (F1) lists every key and mouse action;
  VERSION-HISTORY.txt (next to the .exe) lists everything added so far.

## Coming next

- **v0.10 — HDRI prep:** sun to light (direction, strength, colour for Unreal /
  Blender); calibrate (light level in lux, scale to a measured value or a
  preset, white balance from a grey spot, sRGB / ACEScg); remove the sun; rotate and level the horizon; batch resize (2K / 4K
  / 8K) and convert between EXR and HDR.
- **v0.11 — HDRI library:** favourites, tags, search; sort by brightness, sun
  height or colour temperature; drag an HDRI into Unreal or Blender.
- **v0.12 — Inspect:** histogram, false colour, chrome / grey ball preview,
  metadata panel.
- **v1.0 — Launch:** A/B compare, final testing on a clean Windows 11 PC.
- **Later:** File Explorer thumbnails (optional installer), ARM64, float TIFF /
  PFM, DWAA / DWAB, 360° view export, 16-bit PNG.

## Built with

- C++17 and the Windows API (Win32, GDI, standard file dialogs) — no
  frameworks, one statically linked .exe.
- Compiled with Clang (LLVM-MinGW).
- tinyexr + miniz (OpenEXR), stb_image / stb_image_write (Radiance HDR, PNG, JPEG).
- Python with Pillow and NumPy for the app icon, the logo resource and test tools.

## License

GNU GPL v3 or later ([LICENSE](../LICENSE)); the name and logo are reserved
([TRADEMARKS.md](../TRADEMARKS.md)). Included libraries keep their own licenses
(Help > About > Licenses, [THIRD-PARTY-NOTICES](../release/THIRD-PARTY-NOTICES.txt)):
tinyexr (BSD 3-Clause), miniz (MIT), stb_image / stb_image_write (MIT or
public domain). Versions: [third_party/VERSIONS.txt](../third_party/VERSIONS.txt).

## Contact

Made with ❤︎ from Greece — pixelrise@gmail.com

## Status: v0.9.8 beta

- Open source: GNU GPL v3 or later, on GitHub (p1xelrise/EXRay). Help menu
  links to the source code and the support page; About and Licenses show the
  license; SPDX headers in every source file.

### v0.9.7 beta

- Fix HDRI tested against real photographs: tests/ground_truth_test.cpp takes
  HDRIs with real, unclipped suns, clips copies at peak/100, /40 and /10, fixes
  them and compares the sun light with the original. With the matching sky
  choice the rebuilt sun is within 75–122 % of the real light in all cases.
- Clipped-sun detection: a plateau only counts if no neighbouring pixel is
  brighter (equal values on the slope of a real sun are no longer "clipped").
- Clear-sky sun-to-sky ratio refitted to measured skies: 20·(1 − e^(−h/50)).
- Sky choice Clear / Light haze / Hazy (0 / −1 / −1.5 EV) replaces Strength.
- Clipped areas wider than 2° (hazy or sunset glow) are reported but not
  ticked by default. Default colour: from the glow.
- Sun check: exposure set so the expected real sun is just white; the status
  bar shows the numbers (e.g. "clipped sun 2,000 → rebuilt 126,000").

### v0.9.6 beta

- Fix HDRI: shows the share of the real sun light a clipped sun still holds;
  sun size (0.53° / 1° / 2° / 4°, same total light); sun colour from the glow
  or natural for its height (Planckian locus, CCT by elevation).
- Sun check (S, Tools menu, button in Fix HDRI): 360° view aimed at the sun at
  −10 EV, Clip; S again restores the view and look.
- Help > Version history removed; VERSION-HISTORY.txt ships with the .exe.

### v0.9.5 beta

- Free for everyone: every feature, no time limit, no Pro version (the v0.9.4
  Pro preview is removed). Optional payment on the Gumroad page
  (https://p1xelrise.gumroad.com/l/exray); About links to it.

### v0.9.4 beta

- Pro preview: Fix HDRI, Cryptomatte names / isolate and grid (batch) export
  work until 31 March 2027 (`kPreviewEnd` in main_win.cpp), then show a short
  "EXRay Pro feature" note; everything else is EXRay Free and keeps working.
  `EXRay.exe /preview-ended` shows the app as it will be after the preview.

### v0.9.3

- About: new roadmap and the Free / Pro plan.

### v0.9.2

- Renamed to **EXRay** (EXRay.exe, EXRay.ini; the old EXRay.ini is
  read once if EXRay.ini doesn't exist yet).
- Dark menu bar (painted through the UAH menu messages) and dark popup menus
  (preferred app mode, uxtheme ordinals 135/136); falls back to light menus.
- Fix HDRI: pixel counts as whole numbers.

### v0.9.1

- Own Radiance .hdr reader: no 2 GB limit (stb_image refused images over
  ~180 million pixels, e.g. 24K–32K HDRIs), reads +Y files, and shrinks while
  decoding for thumbnails (a 20000×10000 .hdr: 28 MB instead of 2.3 GB).

### v0.9

- **Fix HDRI** (toolbar Fix, Tools menu, or H) for colour images:
  - Checks the image and reports: clipped sun (flat plateau, position,
    elevation, size), broken pixels (NaN / infinite / negative), hot pixels,
    and "not real HDR" (nothing above 1.0).
  - Clipped sun: rebuilt as a 0.53° disc. Its light is set from the sky
    around it with a clear-sky sun-to-sky ratio for its elevation (about 3.6×
    at 10°, 6.3× at 19°, 11× at 40°), energy-conserving at any resolution.
    Sky: Clear / Light haze / Hazy. An already-bright (unclipped) sun is left alone;
    clipped areas that are too large (windows, overexposed sky) or below the
    horizon are reported, not rebuilt.
  - Broken values take their neighbours' average (negatives become 0); hot
    pixels take their neighbours' median. Many tiny bright points (stars,
    small lights) are not ticked by default.
  - B toggles before / after; Ctrl+S saves the fixed copy as OpenEXR 32-bit
    float, OpenEXR 16-bit half (warns when the sun is above 65,504) or
    Radiance .hdr. The original file is never overwritten; leaving an unsaved
    fix asks first.
- Help > Version history: every version so far, newest first.
- Toolbar a little narrower minimum (slider can shrink) to fit the Fix button.

### v0.8.3

- About: the "Free for everyone, for non-commercial use" line moved under the e-mail.

### v0.8.2

- Version in the window title ("HDR & EXR file preview — v0.8.2",
  "sky_04.exr — HDR & EXR file preview v0.8.2").
- About: "Free for everyone, for non-commercial use…" line; Licenses page with
  the notices of the included libraries.

### v0.8.1

- Help > About: new "Coming next" roadmap, fuller "Built with", and a contact
  block with the pixelrise logo and a clickable e-mail link.

### v0.8

- 360° view (toolbar 360° button or V) for 2:1 equirectangular images: drag to
  look around, wheel / Ctrl+ / Ctrl− for field of view (20–140°), double-click
  or F to reset. Renders at half resolution while moving, full when it stops.
  Exposure, tonemap, layers, pixel readout and Cryptomatte clicks all work in
  it. It stays on while browsing other 2:1 images.

### v0.7

- Help > Controls and shortcuts (F1): a proper window with the keys and their
  descriptions in aligned columns (replaces "Keyboard and mouse").
- Help > About: what the tool does, main features and ideas for next versions.

### v0.6

- Export look: Clip (raw sRGB, exposure 0, the default), ACES (exposure 0) or
  as shown on screen — chosen in both export dialogs and remembered.
- Readable data passes: depth (near white / far black, range found
  automatically, empty background ignored; I inverts), normals (−1..+1 shown as
  normal-map colours), motion vectors / positions (scaled around mid-grey).
  The pixel readout still shows the stored values. Exports save these views.

### v0.5

- Cryptomatte (Unreal Movie Render Queue, Blender, …): one layer per matte
  shown as ID colours; the object/material name under the mouse (from the
  file's manifest); click to isolate an object as a mask, Ctrl+click to add
  more, Esc to show all. Export saves the current view.
- Grid multi-select: Ctrl+click, Shift+click, Shift+arrows, Ctrl+A; right-click
  menu. Ctrl+E exports the selection to a chosen folder (PNG or JPEG, optional
  every layer of multi-layer EXRs), one image at a time, with progress and Esc
  to cancel. Existing files are never overwritten ("name (2).png").
- Grid thumbnail size: slider in the info bar (50–200%), Ctrl+wheel,
  Ctrl+ / Ctrl− / Ctrl+0. Sharper thumbnails are re-made in the background.
- Settings in EXRay.ini next to the .exe (thumbnail size, exposure,
  tonemap, export folder/format, window position). Skipped if the folder is
  read-only.

### Phase 4 (v0.4)

- EXR layers: multi-part, multi-view and render-pass files (e.g. `diffuse.R/G/B`,
  `depth.Z`, `N.X/Y/Z`). Click the layer name in the info bar (or press L) for a
  list; `[` and `]` step through them. Zoom and position are kept when switching.
  RGB layers show in colour, XYZ vectors as RGB, single channels as grey.
- Pixel readout: x, y and the linear R G B values under the mouse, in the info
  bar. Ctrl+C copies them.
- Export (Ctrl+E): PNG or JPEG of the current layer at full resolution, always
  with the default look (ACES, exposure 0), whatever the slider shows.

### Phase 3

- Folder grid: File > Open folder (Ctrl+Shift+O), drop a folder on the window,
  or pass a folder on the command line. Thumbnails fit a 312×156 box (scaled
  with Windows display scaling) and are made in the background, visible tiles
  first, then the rest. Kept in memory only (nothing written to disk).
- Very large images (over 40 MP) are decoded one at a time; small ones carry on
  meanwhile. Thumbnail work pauses while a full image is opening.
- Exposure and tonemap apply to the thumbnails too.
- Grid | Image switch on the toolbar; G / Esc goes from an image to its folder
  grid, Enter / double-click opens. F5 refreshes (unchanged files keep their
  thumbnails). While an image opens, its thumbnail is shown enlarged.

### Phase 2

- Open a file: drag & drop onto the window, File > Open (Ctrl+O), or pass it
  on the command line / drop it onto the .exe.
- Toolbar: exposure slider (-10 to +10 EV), tonemap (Clip / Reinhard / ACES),
  Fit, 100%, previous / next file.
- Mouse wheel zooms at the cursor, drag to pan, double-click toggles Fit / 100%.
- Arrow keys, Home, End browse the .hdr/.exr files in the same folder
  (sorted like File Explorer). Exposure and tonemap stay as set.
- Big images: a 2048 px preview is re-tonemapped live while the slider moves;
  the full-resolution image is redone in the background when it stops.
- Loads on a background thread; Unicode paths (Greek folder names etc.) work.
- Help > Keyboard and mouse lists all shortcuts.

Not supported: deep EXR, luminance/chroma (YC) EXR, DWAA/DWAB compression.

## Build (from Linux)

Needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT build).

    LLVM_MINGW=/path/to/llvm-mingw ./build.sh          # x86_64
    LLVM_MINGW=/path/to/llvm-mingw ./build.sh aarch64  # ARM64

Output: `build/<arch>/EXRay_v<version>.exe` (version from `kVersion` in main_win.cpp)

`tools/package.sh` then makes `release/dist/EXRay_v<version>.zip` (exe, license,
notices, version history, readme) and its SHA-256.

## Tests (Linux)

    tests/build_tests.sh        # builds the test tools into build/linux/
    build/linux/fix_test        # synthetic suns: clipped, rebuilt, energy checked
    build/linux/radiance_test   # own .hdr reader against stb_image

`ground_truth_test` checks Fix HDRI against real HDRIs with unclipped suns. It
was run with these Poly Haven HDRIs (CC0, 8K .hdr from polyhaven.com):
qwantani_afternoon_puresky, qwantani_mid_morning_puresky,
qwantani_late_afternoon_puresky, winter_river, near_the_river_01, aarfontein_pan:

    build/linux/ground_truth_test qwantani_afternoon_puresky_8k.hdr:clear \
        qwantani_mid_morning_puresky_8k.hdr:clear qwantani_late_afternoon_puresky_8k.hdr:clear \
        winter_river_8k.hdr:lighthaze near_the_river_01_8k.hdr:hazy aarfontein_pan_8k.hdr:glow

`light_test <truth.hdr> <out_prefix> <clip level> [strength EV]` renders a test
scene lit by the real HDRI, a clipped copy and the fixed copy.

## Layout

- `src/image_io.*`  – file reading and format detection (tinyexr, stb_image)
- `src/tonemap.*`   – exposure + ACES/Reinhard/clip + sRGB, multi-threaded
- `src/resample.*`  – box-filter downscaling (preview, display pyramid)
- `src/display_win.*` – Windows bitmaps and the display pyramid
- `src/toolbar.*`   – the custom-drawn toolbar
- `src/folder_nav.*` – listing .hdr/.exr files in a folder
- `src/exr_doc.*`   – EXR parts, channels and layers (on tinyexr's low-level API)
- `src/export.*`    – PNG / JPEG export; saving linear EXR / Radiance HDR copies
- `src/batch_export.*` – exporting many files / layers
- `src/settings_win.*` – the .ini settings file
- `src/info_window.*` – the Controls and shortcuts / About window
- `src/pano.*`        – the 360° view renderer
- `src/hdri_fix.*`    – Fix HDRI analysis and repairs (clipped sun, broken / hot pixels)
- `src/fix_window.*`  – the Fix HDRI report window
- `res/logo.bin`      – the logo for the About page (made by tools/make_logo.py)
- `src/thumbnail.*` – thumbnails (area resample, compact RGBE storage)
- `src/thumb_pool.*` – background thumbnail workers
- `src/grid_view.*` – grid layout, scrolling, drawing
- `src/main_win.cpp` – the Win32 window, view, input
- `tests/`           – native test tools (build with tests/build_tests.sh)
- `tools/`           – icon, logo, promo images, landing page, packaging
- `third_party/`     – tinyexr (+ miniz), stb

## Plan

1. ✅ Open one file, tonemap, show it
2. ✅ Exposure, tonemap choice, zoom & pan, arrow-key browsing
3. ✅ Thumbnail grid (312×156) with background loading
4. ✅ EXR layers, pixel readout, export PNG/JPG (default exposure)
5. Large-file and clean-PC testing
