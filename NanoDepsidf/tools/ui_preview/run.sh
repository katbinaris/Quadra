#!/bin/sh
# Renders every Pixel UI screen from the firmware's own drawing code into a PNG contact sheet.
#   tools/ui_preview/run.sh [out.png]     (default: tools/ui_preview/out/preview.png)
# Needs a host C++ compiler and the tools venv (Pillow) for the PPM -> PNG step.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src"
OUT=${1:-"$HERE/out/preview.png"}
BUILD="$HERE/out/build"
mkdir -p "$BUILD/fonts" "$(dirname "$OUT")"

# Copy (not include-path) the sources: a quoted #include resolves next to the including file
# first, which would pick up the real lgfx_config.hpp from src/ instead of the stub.
cp "$SRC"/ui_gfx.cpp "$SRC"/ui_gfx.hpp "$SRC"/ui_screens.cpp "$SRC"/ui_screens.hpp "$SRC"/ui_extras.cpp "$SRC"/ui_extras.hpp \
   "$SRC"/ui_fx.cpp "$SRC"/ui_fx.hpp "$SRC"/ui_vinyl.cpp "$SRC"/ui_vinyl.hpp "$SRC"/user_prefs.h "$SRC"/ui_shape.cpp "$SRC"/ui_shape.hpp "$SRC"/menu.h "$SRC"/pd_status.h "$SRC"/sysmon.h "$SRC"/ui_state.h "$SRC"/haptic_params.h \
   "$SRC"/audio_trigger.h "$SRC"/boot_mode.h "$SRC"/home.h "$BUILD/"
cp "$SRC"/fonts/*.cpp "$SRC"/fonts/*.h "$BUILD/fonts/"
mkdir -p "$BUILD/icons"
cp "$SRC"/app_profiles/icons/*.c "$SRC"/app_profiles/icons/*.h "$BUILD/icons/"
cp "$HERE"/stub/*.hpp "$HERE"/stub/*.h "$BUILD/"
mkdir -p "$BUILD/class/hid" "$BUILD/app_profiles/icons"
cp "$HERE"/stub/class/hid/hid.h "$BUILD/class/hid/"
cp "$SRC"/ui_cards.cpp "$SRC"/ui_cards.hpp "$BUILD/"
cp "$SRC"/app_colors.c "$SRC"/app_colors.h "$BUILD/"
cc -std=gnu11 -O2 -I"$BUILD" -c -o "$BUILD/app_colors.o" "$BUILD/app_colors.c"
cp "$SRC"/app_profiles/app_profile.h "$SRC"/app_profiles/figma.c "$SRC"/app_profiles/plasticity.c "$SRC"/app_profiles/onshape.c "$SRC"/app_profiles/agents.c "$BUILD/app_profiles/"
cp "$SRC"/app_profiles/icons/app_icons.h "$BUILD/app_profiles/icons/"
# Profile data is C (compound literals), so it builds as C and links in.
cc -std=gnu11 -O2 -I"$BUILD" -c -o "$BUILD/figma.o" "$BUILD/app_profiles/figma.c"
cc -std=gnu11 -O2 -I"$BUILD" -c -o "$BUILD/plasticity.o" "$BUILD/app_profiles/plasticity.c"
cc -std=gnu11 -O2 -I"$BUILD" -c -o "$BUILD/onshape.o" "$BUILD/app_profiles/onshape.c"
cc -std=gnu11 -O2 -I"$BUILD" -c -o "$BUILD/agents.o" "$BUILD/app_profiles/agents.c"

# The Figma test icon, converted exactly as send_icon.py sends it, for the "uploaded icon" tile.
"$HERE/../.venv/bin/python" -c "
import sys; sys.path.insert(0, sys.argv[1])
from send_icon import fit_icon, to_rgb565_be
from PIL import Image
open(sys.argv[3], 'wb').write(to_rgb565_be(fit_icon(Image.open(sys.argv[2]))))" \
    "$HERE/.." "$HERE/../icons/figma_pixel_48.png" "$BUILD/icon.raw"

# A made-up cover for the MUSIC tiles (no real artwork): a dusk gradient, a striped sun and
# hills, its lower part darkened as display_task.cpp does under the title.
"$HERE/../.venv/bin/python" -c "
import sys
from PIL import Image, ImageDraw
im = Image.new('RGB', (240, 240)); d = ImageDraw.Draw(im)
for y in range(240):
    t = y / 239
    d.line((0, y, 239, y), fill=(int(40 + 200 * t), int(20 + 70 * t), int(90 - 40 * t)))
d.ellipse((60, 50, 180, 170), fill=(255, 190, 70))
for i, y in enumerate(range(118, 170, 9)):
    d.rectangle((0, y, 239, y + 2 + i), fill=(int(40 + 200 * y / 239), int(20 + 70 * y / 239), int(90 - 40 * y / 239)))
d.polygon([(0, 240), (0, 160), (70, 128), (140, 168), (200, 140), (240, 158), (240, 240)], fill=(30, 16, 50))
plain = bytearray()
for y in range(240):
    for x in range(240):
        r, g, b = im.getpixel((x, y))
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        plain += bytes((v >> 8, v & 255))
open(sys.argv[2], 'wb').write(plain) # as decoded, for the record layouts
out = bytearray()
for y in range(240):
    t = 0 if y < 148 else 1 if y >= 188 else (y - 148) / 40
    k = 1 - 0.76 * t * t * (3 - 2 * t)
    for x in range(240):
        r, g, b = im.getpixel((x, y))
        v = (int((r >> 3) * k) << 11) | (int((g >> 2) * k) << 5) | int((b >> 3) * k)
        out += bytes((v >> 8, v & 255))
open(sys.argv[1], 'wb').write(out)" "$BUILD/cover.raw" "$BUILD/cover_plain.raw"

c++ -std=c++17 -O2 -I"$BUILD" -o "$BUILD/preview" "$HERE/preview.cpp" \
    "$BUILD"/ui_gfx.cpp "$BUILD"/ui_screens.cpp "$BUILD"/ui_extras.cpp "$BUILD"/ui_fx.cpp "$BUILD"/ui_vinyl.cpp "$BUILD"/ui_shape.cpp "$BUILD"/ui_cards.cpp "$BUILD"/fonts/*.cpp "$BUILD/figma.o" "$BUILD/plasticity.o" "$BUILD/onshape.o" "$BUILD/agents.o" "$BUILD/app_colors.o" \
    -x c++ "$BUILD"/icons/*.c
ICON_RAW="$BUILD/icon.raw" COVER_RAW="$BUILD/cover.raw" COVER_PLAIN_RAW="$BUILD/cover_plain.raw" "$BUILD/preview" > "$BUILD/preview.ppm"
"$HERE/../.venv/bin/python" -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[2])" \
    "$BUILD/preview.ppm" "$OUT"
echo "wrote $OUT"
