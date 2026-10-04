# Quadra pixel art: how it is made

This document is the rulebook for everything Quadra draws: the device's screens, the
companion app (which has its own style, section 10), and the screenshots in the docs. It is
for contributors, and for Claude or any other assistant working on the repo. Read it before adding or changing a screen, a sprite, an
icon, a command card or an animation, and follow it exactly. A change that breaks a rule here
will be asked to change.

Paths below are relative to `NanoDepsidf/` unless they start with `companion/`.

## Contents

1. [The rules](#1-the-rules)
2. [Palette](#2-palette)
3. [The canvas](#3-the-canvas)
4. [Drawing primitives](#4-drawing-primitives)
5. [Text](#5-text)
6. [1-bit sprites](#6-1-bit-sprites)
7. [App icons](#7-app-icons)
8. [Command cards](#8-command-cards)
9. [Animation](#9-animation)
10. [The companion app](#10-the-companion-app)
11. [Screenshots for the docs](#11-screenshots-for-the-docs)
12. [Checklist](#12-checklist)

---

## 1. The rules

1. **Whole pixels only.** Every shape is made of axis-aligned, whole-pixel rectangles. No
   anti-aliasing, no blending, no sub-pixel positions, no smoothed lines or curves. A curve or
   a circle is a run of whole pixels.
2. **Black background, four colours.** White, grey, dark and amber (section 2). App icons are
   the only full-colour artwork, apart from the fixed exceptions listed in section 2. A new
   colour needs a reason good enough to change this document.
3. **Drawn from descriptions, not stored as pictures.** Screens, sprites, command cards and
   animations are code or data that describe shapes (a box here, a row there, a keyframe
   time), drawn at runtime. Do not add pre-rendered frames, sprite sheets or bitmaps for UI
   art. The exceptions are the app icons (section 7) and the font (section 5), both generated
   from source files by tools in the repo, and MUSIC's album cover, a picture the computer
   sends (section 2).
4. **Scale by whole numbers.** 2×, 3× and 4× repeat each pixel. A 1.5× version of a sprite is
   a separate hand-drawn sprite (the `_M` sprites), never a resample.
5. **Pixels are shared by firmware and host.** All device drawing goes through `ui_gfx.cpp`,
   which also compiles on the computer for the preview tool (section 11). Do not draw with
   LovyanGFX calls directly in a screen; add a primitive to `ui_gfx` if one is missing.
6. **Time, not frame counts.** Animations are functions of elapsed milliseconds
   (section 9), so they look the same at any frame rate.

## 2. Palette

Defined in `src/ui_gfx.hpp` (firmware) and `companion/src/ui/draw.ts` (`C`, where the companion draws the knob's screen).

| Name | RGB | Used for |
|---|---|---|
| BLACK | `#000000` | Background. Also "transparent" in icons |
| WHITE | `#FFFFFF` | Values, active content |
| GREY | `#6E6E6E` | Labels, hints, inactive items, muted settings |
| DARK | `#3A3A3A` | Rules, empty parts of bars and gauges, outlines |
| AMBER | `#FFC94D` | Focus, editing, pressed keys, unsaved changes, peaks and warnings |

The keycap sprite has its own shading (`KEY_FACE`, `KEY_SIDE`, `KEY_OFF_FACE`,
`KEY_OFF_SIDE`). Those belong to the keycap only.

Each app also has three **accent colours**, used by the idle screen and the LEDs: the
profile's `accents`, or failing that the three most common colours of its 48×48 icon
(`src/app_colors.c`). Accents never appear in menus or general UI.

**Fixed exceptions** (`src/ui_extras.cpp`). These came in with MUSIC, AGENTS, LIGHTS and HOME, and
each is limited to its own screen:
- **Agent requests:** the agent's colour (badge, rim, ring arcs); `ALLOW_GREEN` for the F1
  hold arc and the ALLOW label; `DENY_RED` for the DENY label; a near-black `PANEL` behind the
  command.
- **MUSIC now playing:** the album cover, a 240×240 picture decoded on the device. FLAT
  draws it full screen and darkened under the title. The record styles (`src/ui_vinyl.cpp`)
  average it down to a label and a sleeve, and draw the record per pixel in a **10-step grey
  ramp** (`#000000` to `#FFFFFF`) with 4×4 ordered dither between steps. These are the only
  places the screen shows something that isn't drawn from shapes, and the ramp is used
  nowhere else. The label turns nearest neighbour, in whole pixels. The volume ring takes the
  cover's colours.
- **LIGHTS:** the colour swatch, and the rim that mirrors the LED ring, in the LEDs' own
  colours.
- **HOME:** a lamp's own colour (its colour, or its white as RGB from the colour temperature),
  dimmed with its brightness, for the part of its icon that gives light (glass, light bar, LEDs)
  and the light it throws (a 4×4 ordered-dither cone or glow, whole pixels); and Xiaomi's orange `#FF6900` for the 9×9 Mi
  badge in the header, only there. A lamp that's off or unplugged keeps to the palette. The
  scale being turned is on the LED ring, not the screen.

Everything else on those screens stays in the palette and in whole pixels.

## 3. The canvas

- The display is a round 240×240 panel. Everything is drawn into one full-screen sprite and
  pushed whole. The centre is `ui::CX`, `ui::CY` = (120, 120).
- The corners are outside the glass. Keep content inside the circle; text and rows near the
  top and bottom must be narrower than the circle is at that height.
- Screens are laid out in absolute pixel coordinates. Existing screens in
  `src/ui_screens.cpp` are the reference for spacing: a header at the top, content in the
  middle column (about x 85 to 155 is free on the Haptics ring), hints and the key legend at
  the bottom.

## 4. Drawing primitives

From `src/ui_gfx.hpp`. Use these; they are the only things that touch the frame.

| Primitive | Draws |
|---|---|
| `rect(x, y, w, h, c)` | A filled rectangle |
| `cut(x, y, w, h, c)` | A filled rectangle with its four corner pixels cut |
| `frame_box(x, y, w, h, c)` | A 1px outline with the corners cut |
| `disc(cx, cy, r, c)` | A filled circle in whole pixels |
| `sprite(s, x, y, c, scale)` | A 1-bit sprite (section 6) in one colour |
| `image565(x, y, w, h, data, brightness, scale)` | A full-colour icon; black pixels are skipped |
| `text(s, x, y, c, scale, align)` | Text (section 5) |
| `plot_curve(...)` / `wave_y(...)` | The haptic force curves, 1px or 2px thick |

The **cut corner** is Quadra's signature: boxes, cards, keycaps and buttons all lose their
four corner pixels. Use `cut` and `frame_box` rather than plain rectangles for anything that
reads as an object.

Inactive full-colour icons are dimmed with `brightness`, not tinted grey.

## 5. Text

- The font is **Silkscreen** (OFL), rasterised without anti-aliasing by
  `tools/gen_silkscreen_font.py` into `src/fonts/`. Scale 1 is a 10px font (caps 7px), a
  classic 5×7 console face; scales 2, 3 and 4 are the 8px font doubled, tripled and
  quadrupled.
- Text is **upper case**.
- Use `fit_scale()` to pick the largest scale that fits a width. Alignment uses the ink
  bounds, so centred text is optically centred.
- Labels are GREY, values WHITE, a focused or edited value AMBER. Small captions under a
  label carry the engineering name (`SNAP` / `KP`).
- Short words beat long ones: the ring and the status bar have very little room. `--` means
  "not used here".

## 6. 1-bit sprites

A sprite is a width, a height and a string of `w × h` characters, row after row: `#` is a
set pixel, anything else is empty. They live in `src/ui_gfx.cpp`:

```c
const Sprite SPR_SHAPE_M = {11, 6,
    ".........##" "........###" ".......###." ".....####.." "#######...." "#####......"};
```

- Draw them at the size they will be shown. Most screen icons exist at 1× and as a
  hand-drawn 1.5× (`_M`) version.
- Strokes are 1px at 1×; at 1.5× use 2px where a 1px line would look thin next to the
  other icons.
- Declare new sprites in `ui_gfx.hpp` and add them to the size check in
  `tools/ui_preview/preview.cpp`, which fails if a string has the wrong length.

## 7. App icons

Each app profile has a 24×24 icon (status bar, lists) and a 48×48 icon (profile screen, idle
screen). They are the only full-colour art.

1. **Draw them by hand as pixel art** at exactly 24×24 and 48×48, as PNGs in
   `tools/icons/<app>_pixel_24.png` and `<app>_pixel_48.png`. Draw each size separately;
   don't scale one from the other. Few colours, hard edges, transparent background. No
   anti-aliased edges and no gradients.
2. **Convert them** with `tools/gen_icon_c.py`, which writes RGB565 big-endian C arrays to
   `src/app_profiles/icons/`. It does not resample: what you drew is what the device shows.
   Pixels under 50% alpha become black, which the display treats as transparent.
3. The 48×48 icon also sets the app's **accent colours** unless the profile names its own
   `accents`. Check the idle screen and the LEDs after changing an icon.

Uploaded icons (the companion's Import image, `tools/send_icon.py`) are the exception to
"hand-drawn": they are fitted from any picture. Built-in profiles always use drawn icons.

## 8. Command cards

A command card is the 120×64 animated illustration of what a command does. It is a **scene
description** in the profile's C file (or its JSON), never an image. `src/ui_cards.cpp`
draws it.

- A scene is an optional `base` (drawn under every frame) and a list of **keyframes**. Each
  keyframe has a duration in ms and a handful of elements. The card cuts from one keyframe
  to the next with no tweening, loops, and holds the last one.
- Elements (`app_profile.h`, `EL_*` macros): `EL_BOX` and `EL_FRAME` (cut-corner boxes),
  `EL_DASH` (a group), `EL_DISC`, `EL_SEL` (selection with handles), `EL_LABEL`, `EL_LINE`,
  `EL_ROW` (a layers / outliner / feature-list row), `EL_ISO` and `EL_ISO_FILLET`
  (isometric boxes), `EL_MODES` (Plasticity's selection modes), `EL_ARC` / `EL_ELLIPSE`,
  `EL_NUM`, `EL_ROLLBACK`, `EL_CLIPBOARD`, `EL_PLAY`.
- Colours are the palette's: `K_D` dark, `K_G` grey, `K_W` white, `K_A` amber, `K_B`
  black.
- Show the **result** of the command in two to four keyframes, the way the app itself would
  show it: before, the action (amber marks what changes), after.
- Keep to the layout of the app's existing cards: Figma cards are a mini canvas plus a layers
  panel; Plasticity and Onshape cards are an isometric viewport plus the outliner or the
  feature list.

## 9. Animation

- Every animation is a **pure function of elapsed time**: given `t_ms`, draw the frame. No
  state carried between frames, no frame counters. `src/ui_fx.cpp` (the loading screen and
  the idle routines) and the FEEL / SHAPE / STEPS animations in `src/ui_screens.cpp` are the
  examples to copy.
- Positions are rounded to whole pixels before drawing. Squash and stretch scale nearest
  neighbour.
- Motion has weight: ease in and out (`ease_out3` in `display_task.cpp`), overshoot, squash,
  shake, dust. Avoid linear sliding except for scrolling content.
- Effects (sparks, rings, sparkles) use the palette or the app's accent colours, as single
  pixels or small blocks.
- The display only draws while something changes. If your screen animates, tell
  `display_task.cpp` to keep drawing while it is shown (the `looping` condition in
  `update_ui()`).

## 10. The companion app

The companion (`companion/`) is a desktop app with a lot of forms, so since October 2026 it
has its own style (called B in the design work): a hardware-editor look, readable at desktop
sizes. The pixel rules above apply only where it shows **the knob itself**.

- **Colours:** tokens on `:root` in `companion/src/style.css`. Layered dark surfaces
  (`--bg #0b0c0d`, `--s1`, `--s2`, `--s3`), lines `--line`, text `--text` / `--muted` /
  `--faint`, and the device's **amber** `#FFC94D` for what is chosen, edited or not saved yet
  (a selected card's frame, an amber dot for a live change). Green only for "connected" and
  "in use", red only for destructive buttons.
- **Type:** Geist (UI) and Geist Mono (numbers, ids), bundled in `companion/src/fonts/` (the
  app's CSP blocks web fonts). Sentence case, as in this sentence; names the user sees on the
  knob itself (a profile's name, key labels, the idle word) keep their capitals.
- **Shapes:** rounded boxes (12 px), cards (10 px), buttons and fields (7 px), 1 px lines.
  Sliders are a track with a thumb and a value box you can type in. Feels and steps are drawn as
  smooth SVG (`Wave`, `Dial` in `companion/src/ui/controls.tsx`), from the device's own
  `waveY`.
- **Where it stays pixel art:** the knob's main screen drawn from data (`drawMainScreen` in
  `companion/src/ui/draw.ts`: whole pixels, Silkscreen, the device palette; in the sidebar's
  picture and a profile's preview), profile icons (48 × 48 shown at whole multiples with
  `image-rendering: pixelated`; smaller only as a thumbnail, e.g. the sidebar's; black is
  see-through, as on the device, so no black tile shows behind them), and the idle
  word field (Silkscreen).
- **The device picture** (`companion/src/assets/device.png`) is a rendered photo of the
  hardware and the one non-pixel image; the sidebar shows it small, with the knob's screen
  drawn on its glass. The app doesn't stream the knob's screen or LEDs.
- When the device gets a new screen or setting, the companion shows it with the same name
  (in sentence case) and the same order, and greys out what doesn't apply (`—`).

## 11. Screenshots for the docs

- **Device screens** come from the preview tool, never from photos:
  `tools/ui_preview/run.sh out.png` builds the real UI code for the host and renders every
  screen into a contact sheet, printing the tile names in order. Each doc image is one tile:
  the 480×480 (2×) circle, with transparent corners, saved to `docs/images/`. To add one,
  add a `keep("name")` tile to `tools/ui_preview/preview.cpp`.
- **Companion screenshots** come from demo mode, all in one go: with `pnpm dev` running in
  `companion/`, `node scripts/screenshots.mjs` (Playwright: WebKit, the Mac app's engine, at
  1280×800 and a device scale of 2; Chromium for the not-connected page). They live in
  `companion/docs/`. Each page has an address for a single shot, e.g.
  `http://localhost:1420/?demo#/profile/figma/keys/f1`. The demo knob has the firmware's
  built-in profiles and icons (`companion/scripts/gen_demo_builtins.sh`), so the shots show
  what a real knob shows; rerun it before retaking them after a built-in profile or icon
  changes.
- **Illustrations** in the companion's docs (`companion/docs/fig-*.svg`) are hand-written SVG
  in the companion's style: its colours on `#0b0c0d`, Geist (with system fallbacks), sentence
  case. The window map (`fig-window.svg`) is the exception: `screenshots.mjs` makes it from the
  Mode page with numbered callouts, so it is always the real window.
- After any UI change, re-render the screenshots it affects in the same change.

## 12. Checklist

Before committing anything visual:

- [ ] Only the five palette colours (plus an app's own icon and accents, and the fixed
      exceptions in section 2, where they belong). The companion's own UI follows section 10.
- [ ] No anti-aliasing, blending, gradients, sub-pixel positions or smooth scaling.
- [ ] Drawn from code or scene data; no new bitmaps except hand-drawn app icons.
- [ ] Sprites and icons drawn at the size they are shown; 1.5× versions hand-drawn.
- [ ] Text upper case, in Silkscreen, sized with `fit_scale` where space is tight.
- [ ] Animations are functions of `t_ms`, rounded to whole pixels.
- [ ] Everything stays inside the round glass.
- [ ] `tools/ui_preview/run.sh` builds and the new screen looks right in the contact sheet.
- [ ] The companion shows the same thing by the same name, if it shows it at all; its
      drawing of the knob's screen (`ui/draw.ts`) still matches the device.
- [ ] Affected screenshots re-rendered.
