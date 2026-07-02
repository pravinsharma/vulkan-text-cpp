# Firefox Font Handling and Text Rendering

## URLs
GitHub:
- [Source code](https://github.com/mozilla/gecko-dev/tree/main/gfx/thebes)
- [Font handling documentation](https://developer.mozilla.org/en-US/docs/Web/CSS/font)
- [Skia reference](https://skia.org/docs/)

## Overview

Firefox uses a layered font stack: platform font discovery, font matching, HarfBuzz shaping, FreeType/DirectWrite/CoreText rasterization, and Skia for final rendering. Color fonts, including COLRv1 and emoji, are handled through specialized pipeline extensions.

## Architecture Layers

### 1. Font Discovery

`gfx/thebes/SharedFontList.cpp` and platform-specific list builders maintain the available font catalog.

- Windows: `gfx/thebes/gfxDWriteFontList.cpp` uses DirectWrite to enumerate system fonts.
- macOS: `gfx/thebes/CoreTextFontList.cpp` uses CoreText.
- Linux/Android: `gfx/thebes/gfxFcPlatformFontList.cpp` uses fontconfig.
- Android: `gfx/thebes/AndroidSystemFontIterator.cpp` handles system font iteration.

The shared list provides family-name lookup, style matching, and fallback ordering.

### 2. Font Matching

`gfx/thebes/gfxFontEntry.cpp` and `gfx/thebes/gfxFont.h` implement font selection rules.

- `gfxFontEntry` represents one physical font face.
- `gfxFont::ShapeText` selects a font for a text run, applying CSS font properties.
- `gfxFontGroup` manages multiple `gfxFontEntry` instances for fallback.

When a glyph is missing from the primary font, Firefox walks the fallback list to find a compatible face.

### 3. FreeType Integration (Linux/Android)

`gfx/thebes/gfxFT2Fonts.cpp` and `gfx/thebes/gfxFT2FontBase.cpp` wrap FreeType faces.

- `gfxFT2Font` owns an `FT_Face` and a HarfBuzz font (`hb_font_t`).
- `FillGlyphDataForChar` loads a glyph index via `FT_Get_Char_Index` and `Factory::LoadFTGlyph`.
- `AddRange` performs per-glyph shaping for fallback paths: glyph lookup, optional kerning via `FT_Get_Kerning`, advance conversion from 26.6 fixed-point, and `lsb_delta`/`rsb_delta` adjustment.
- `gfx/thebes/gfxFT2Utils.cpp` contains helper routines for FreeType metrics and font enumeration.

### 4. HarfBuzz Shaping

`gfx/thebes/gfxFT2Fonts.cpp:ShapeText` calls `gfxFont::ShapeText`, which invokes HarfBuzz through `gfx/thebes/gfxTextRun.cpp`.

- `hb_buffer_t` collects UTF-16 or UTF-8 text.
- `hb_shape()` produces positioned glyphs, including contextual forms, ligatures, and combining marks.
- If HarfBuzz fails, Firefox falls back to the raw FreeType glyph path in `AddRange`.

`gfx/thebes/gfxFontFeatures.cpp` maps OpenType feature tags onto HarfBuzz feature lists.

### 5. DirectWrite / CoreText (Windows / macOS)

On Windows, `gfx/thebes/gfxDWriteFonts.cpp` wraps DirectWrite `IDWriteFontFace`.

- Glyph indices and advances come from DirectWrite.
- `ShouldHintMetrics` decides whether to use hinted or unhinted advances based on platform zoom policy.

On macOS, `gfx/thebes/gfxCoreTextFonts.cpp` and `gfx/thebes/gfxCoreTextShaper.cpp` use CoreText for shaping and metrics.

### 6. Skia Rendering

Modern Firefox Compositor/WebRender renders through Skia (`gfx/skia`).

- `SkFontMgr` discovers typefaces; `SkTypeface` represents a font face.
- `SkStrikeCache` caches rasterized glyphs.
- Skia draws glyphs as paths or bitmaps; on GPU backends it may cache glyph textures.
- Firefox does not use SDF/MSDF for browser text. Crispness comes from native rasterization, hinting, and subpixel rendering.

### 7. Glyph Caching

Rasterized glyphs are cached in memory and/or GPU textures. The cache keys on font, size, and glyph index. Invalidation happens on font changes, size changes, or color-glyph format switches.

## COLRv1 Color Font Support

`gfx/thebes/COLRFonts.cpp` and `gfx/thebes/COLRFonts.h` implement COLR/CPAL table handling.

### Supported Formats

- COLRv0: layered glyphs from multiple font layers.
- COLRv1: Paint-based vector descriptions using Paint records.
- CBDT/CBLC: embedded bitmap glyphs.
- SVG table: vector glyphs rendered via SVG.
- SBIX: Apple bitmap color glyphs.

### Pipeline

1. Font loading detects `COLR`, `CPAL`, `CBDT`, `SVG`, and `SBIX` tables.
2. `COLRFonts.cpp` parses palette definitions and paint records.
3. For COLRv1, Firefox builds a paint tree per glyph.
4. Rendering dispatches to Skia or platform APIs that understand color glyphs.
5. Emoji presentation selectors (U+FE0F) trigger color-glyph codepoint lookup and COLR/SVG rendering instead of monochrome outlines.

### COLRv1 Painting Model

COLRv1 defines vector painting operators: Fill, Stroke, Glyph, Transform, Translate, Scale, Rotate, Skew, Composite, and Clip. Firefox translates these into Skia draw operations or into platform-native paint records.

## Emoji Handling

Emoji rendering crosses font fallback, shaping, and color-glyph paths.

- `gfxFontGroup` performs fallback for emoji codepoints, preferring color-font faces that advertise emoji support.
- `gfx/thebes/gfxFontUtils.cpp` detects emoji presentation selectors and variation sequences.
- When a font provides COLR, CBDT, or SVG tables for an emoji, Firefox prefers that face for the codepoint.
- If no color font covers the emoji, Firefox falls back to a bitmap or vector glyph and renders it in the text color.

## Metrics, Hinting, and Subpixel Rendering

- `gfx/thebes/gfxFont.h` defines metrics: em-height, ascent, descent, line gap, x-height, cap-height.
- `ShouldHintMetrics` in `gfxFT2Fonts.cpp` and platform equivalents decide whether to snap glyph positions to pixel boundaries.
- On LCD subpixel-aware backends, Firefox enables subpixel AA unless overridden by user preferences.

## Anti-aliasing and Gamma

- `gfx/thebes/gfxAlphaRecovery*.cpp` implements alpha recovery for subpixel text on transparent backgrounds.
- Gamma-correct blending is applied where the platform compositor requires it.

## Summary for Vulkan Projects

If you want a Firefox-grade text stack in a Vulkan app without Skia:

1. Use HarfBuzz for shaping, matching Firefox's `ShapeText` flow.
2. Use FreeType for rasterization, with FreeType hinting flags tuned per platform.
3. Implement glyph caching and atlas packing yourself.
4. For color fonts, parse COLR/CPAL/CBDT/SVG tables and branch rendering to color-aware paths.
5. For emoji, detect U+FE0F and prefer color-font faces in the fallback chain.
