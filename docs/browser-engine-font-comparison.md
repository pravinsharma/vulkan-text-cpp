# Browser Engine Font Handling: Chromium, Firefox, WebKit, and Opera Presto

## Source Code Availability

| Engine | Source Available | Repository |
|--------|-----------------|------------|
| Chromium | Yes | https://github.com/chromium/chromium |
| Firefox | Yes | https://github.com/mozilla/firefox |
| WebKit | Yes | https://github.com/WebKit/WebKit |
| Opera Presto | No | Proprietary, never open-sourced |

Opera's original Presto engine source code was never released publicly. Opera kept it proprietary until 2013, when they switched to Chromium/Blink. All information about Presto is from published papers, patents, and reverse-engineering.

## Shared Foundations

All modern browser engines converge on the same core libraries:
- **FreeType** — low-level rasterization
- **HarfBuzz** — text shaping (OpenType + AAT)
- **Skia** or **Cairo** — 2D rendering

| Component | Firefox | Chromium | WebKit |
|-----------|---------|----------|--------|
| FreeType | Bundled (`modules/freetype2`) | Bundled (`third_party/freetype`) | `Source/WebCore/platform/graphics/freetype` |
| HarfBuzz | `gfx/harfbuzz` + `gfx/harfbuzz_glue` | Bundled (`third_party/harfbuzz`) | `Source/WebCore/platform/graphics/harfbuzz` |
| Rendering | Skia + WebRender (`gfx/skia`) | Skia + Skia Graphite (`ui/gfx`) | Skia / CoreText (`platform/graphics/skia`, `coretext`) |
| Platform APIs | DirectWrite (Win), CoreText (mac), fontconfig (Linux) | DirectWrite (Win), CoreText (mac), fontconfig (Linux) | CoreText (Apple), FreeType (Linux/WPE) |

## Architecture Comparison

### Chromium/Blink

**Source:** `chromium/chromium` (`ui/gfx`, `third_party/blink`, `third_party/skia`)

**Font stack:**
- `third_party/freetype` — universal rasterizer on all platforms, including Windows and macOS
- `third_party/harfbuzz` — bundled HarfBuzz 14.2.1-22, intentionally excludes platform backends (`hb-coretext`, `hb-directwrite`, `hb-uniscribe`)
- `ui/gfx/font_fallback_*.cc` — platform-specific fallback for native UI text
- `third_party/blink/renderer/platform/fonts` — web text shaping pipeline
- `ui/gfx/canvas_skia.cc` — Skia canvas wrapper for native UI text

**Key characteristics:**
- Uses FreeType everywhere, not just Linux
- `ui/gfx` handles native UI fonts; Blink handles web content fonts separately
- Early COLRv1 adopter (Chrome 98, Feb 2022)
- Skia Graphite backend (Chrome 113+) as newer GPU-preferred path
- SDF used in some contexts (Chrome OS UI)

### 1. FreeType Scope and Platform Usage

**Firefox:**
- Uses FreeType primarily on Linux/Android through `gfx/thebes/gfxFT2Fonts.cpp`.
- On Windows, uses DirectWrite (`gfx/thebes/gfxDWriteFonts.cpp`) as the primary rasterizer.
- On macOS, uses CoreText (`gfx/thebes/gfxCoreTextFonts.cpp`).
- FreeType is mainly a fallback/enabler for OpenType Variations on older OS versions.

**Chromium:**
- Uses FreeType on **all** platforms, including Windows and macOS.
- FreeType is used for PDFium and for enabling font format support across platforms.
- `third_party/freetype/README.chromium` states: "FreeType is needed on Windows and Mac for PDFium as well for enabling font format support for OpenType Variations on older OS versions."
- More aggressive about using FreeType as a universal rasterizer rather than relying solely on platform APIs.

### 2. HarfBuzz Integration and Build Strategy

**Firefox:**
- Uses HarfBuzz through `gfx/thebes/gfxFT2Fonts.cpp:ShapeText`.
- Calls `gfxFont::ShapeText`, which invokes HarfBuzz.
- Falls back to raw FreeType glyph path (`AddRange`) if HarfBuzz fails.
- Builds most HarfBuzz components, including platform-specific backends.

**Chromium:**
- Bundles HarfBuzz 14.2.1-22 (newer than Firefox's version).
- **Intentionally excludes** platform-specific HarfBuzz backends:
  - `hb-coretext.h/cc` — relies on HarfBuzz's built-in AAT shaping instead
  - `hb-directwrite.h/cc` — uses FreeType path instead
  - `hb-uniscribe.h/cc` — uses FreeType path instead
  - `hb-fallback-shape.cc`
  - `hb-ot-color-sbix-table.hh`
  - `hb-ot-color-svg-table.hh`
- This is a deliberate architectural choice documented in `third_party/harfbuzz/README.chromium`: "Currently we are intentionally not building the following files from HarfBuzz. Specifically, we are not building hb-coretext any longer, as we rely on HarfBuzz' built-in AAT shaping."

### 3. Font Discovery and Fallback Architecture

**Firefox:**
- `gfx/thebes/SharedFontList.cpp` + platform-specific builders:
  - `gfxDWriteFontList.cpp` (Windows)
  - `CoreTextFontList.cpp` (macOS)
  - `gfxFcPlatformFontList.cpp` (Linux)
  - `AndroidSystemFontIterator.cpp` (Android)
- `gfxFontEntry` represents physical font faces.
- `gfxFontGroup` manages fallback chains.
- Fallback is tightly coupled with shaping pipeline.

**Chromium:**
- `ui/gfx/font_fallback*.cc` with platform-specific implementations:
  - `font_fallback_linux.cc`
  - `font_fallback_mac.mm`
  - `font_fallback_win.cc`
  - `font_fallback_skia_impl.cc/h` — Skia-based fallback
- `ui/gfx/font_list.cc` manages font catalogs.
- More separated architecture: `ui/gfx` handles native UI fonts, Blink handles web text.
- `ui/gfx/font.h` defines `gfx::Font` with platform-specific `PlatformFont` wrapper.
- Has `GetFallbackFonts()` and `GetFallbackFont()` as explicit public APIs.

### 4. Color Fonts and Emoji Handling

**Firefox:**
- `gfx/thebes/COLRFonts.cpp` and `COLRFonts.h` implement COLR/CPAL table handling.
- Supports: COLRv0, COLRv1, CBDT/CBLC, SVG table, SBIX.
- Detects emoji presentation selectors (U+FE0F) and variation sequences.
- Prefers color-font faces (COLR, CBDT, SVG) for emoji codepoints.
- Renders color glyphs through Skia or platform APIs.

**Chromium:**
- **Early and aggressive COLRv1 adopter**: Chrome 98 (Feb 2022) was the first stable browser to ship COLRv1 support.
- Supports: COLRv0, COLRv1, CBDT/CBLC, sbix, SVG-in-OpenType.
- Uses FreeType for formats that don't work natively on Windows/macOS.
- Has `third_party/emoji-metadata` and `third_party/emoji-segmenter` for emoji handling.
- Uses `nanoemoji` tooling for building/test color fonts.
- Skia Graphite (newer backend) improves color font rendering performance.

### 5. Rendering Backend

**Firefox:**
- Uses Skia as rendering backend through `gfx/skia`.
- `SkStrikeCache` caches rasterized glyphs.
- No SDF/MSDF for browser UI text.
- Crispness comes from native rasterization, hinting, and subpixel rendering.
- WebRender is the compositor; Skia draws into surfaces.

**Chromium:**
- Also uses Skia, but with **Skia Graphite** as a newer GPU-preferred backend (Chrome 113+).
- More aggressive about GPU-accelerated text rendering.
- Uses SDF in some UI contexts (Chrome OS UI).
- `ui/gfx/canvas_skia.cc` wraps Skia canvas for native UI drawing.
- Blink uses Skia directly for web content rendering.

### 6. Text Shaping Pipeline

**Firefox:**
```
Text Input → gfxFont::ShapeText → HarfBuzz → gfxShapedText → Skia Render
                                  ↓ (fallback)
                             FreeType AddRange
```
- `gfx/thebes/gfxTextRun.cpp` manages shaped text runs.
- `gfxFontFeatures.cpp` maps OpenType features to HarfBuzz.
- shaped text is stored in `gfxShapedText` with compressed/detailed glyph records.

**Chromium:**
```
Web Text: Blink → HarfBuzz → Skia → GPU/CPU
Native UI: ui/gfx → platform font → Skia → GPU/CPU
```
- Blink has its own text runner (`third_party/blink/renderer/platform/fonts`).
- HarfBuzz is deeply integrated into Blink's font system.
- `ui/gfx` handles native UI text separately.
- More parallel paths for web vs. native text.

### 7. Metrics, Hinting, and Subpixel

**Firefox:**
- `gfx/thebes/gfxFont.h` defines metrics: ascent, descent, line gap, x-height, cap-height.
- `ShouldHintMetrics` decides hinted vs. unhinted advances.
- `gfx/thebes/gfxAlphaRecovery*.cpp` implements alpha recovery for subpixel text.
- Platform-specific rendering params in `gfx/thebes/`.

**Chromium:**
- `ui/gfx/font_render_params*.cc` has extensive platform-specific tuning:
  - `font_render_params_linux.cc`
  - `font_render_params_mac.cc`
  - `font_render_params_skia.cc`
- `FontRenderParams` struct controls hinting, antialiasing, subpixel rendering.
- More granular control over rendering parameters per-platform.

### 8. Android-Specific Handling

**Firefox:**
- `gfx/thebes/AndroidSystemFontIterator.cpp` for system font iteration.
- `gfx/thebes/gfxAndroidPlatform.cpp` for Android font platform.
- Uses FreeType + HarfBuzz on Android.

**Chromium:**
- More extensive Android font infrastructure due to Chrome's larger Android footprint.
- `third_party/emoji-metadata` for emoji segment detection.
- `third_party/emoji-segmenter` for emoji boundary detection.
- Android-specific font fallback in `ui/gfx/font_fallback_linux.cc` (shared with Linux).

## WebKit/Safari

**Source:** `WebKit/WebKit` (`Source/WebCore/platform`)

**Font stack:**
- Native platform APIs primary: CoreText on macOS/iOS, FreeType on Linux/WPE
- `Source/WebCore/platform/graphics/coretext` — CoreText integration
- `Source/WebCore/platform/graphics/freetype` — FreeType integration
- `Source/WebCore/platform/graphics/harfbuzz` — HarfBuzz shaping (`ComplexTextControllerHarfBuzz.cpp`)
- `Source/WebCore/platform/text/cf` and `cocoa` — text shaping on Apple platforms
- `Source/WebCore/platform/graphics/vulkan` — Vulkan rendering backend

**Key characteristics:**
- Tightest integration with platform-native text stacks
- CoreText does much of the heavy lifting on Apple platforms
- `ComplexTextControllerHarfBuzz.cpp` handles shaping when CoreText/AAT doesn't cover a script
- Skia is rendering backend; also supports Vulkan (`platform/graphics/vulkan`)
- FreeType used on GTK/WPE/Linux
- Strong support for color fonts via CoreText/SwiftShader

**Source files:**
- `Source/WebCore/platform/text/ComplexTextController.cpp` — complex text layout
- `Source/WebCore/platform/graphics/freetype/FontCacheFreeType.cpp` — FreeType font cache
- `Source/WebCore/platform/graphics/harfbuzz/ComplexTextControllerHarfBuzz.cpp` — HarfBuzz shaping integration
- `Source/WebCore/platform/graphics/coretext/` — CoreText-specific glyph rasterization
- `Source/WebCore/platform/graphics/freetype/SimpleFontDataFreeType.cpp` — FreeType font data

### Opera Presto (Historical)

**Status:** Source code **never released**. Opera developed Presto internally from 2003–2013 before switching to Chromium/Blink.

**Known characteristics** (from public statements, patents, and testing):
- Proprietary text shaping engine, predating HarfBuzz's dominance
- Used Pango for Linux text layout in later versions
- Strong Unicode/bidi support early on
- No public source for COLR/color fonts; emoji support added late via platform fallback
- SDF not used in browser text; relied on platform rasterization

**Why it matters:** Presto is not a viable reference for modern open-source font handling. All current Opera browsers use Chromium/Blink.

## Color Fonts and Emoji

| Engine | COLRv1 | CBDT/CBLC | SVG-in-OT | sbix | Emoji handling |
|--------|--------|-----------|-----------|------|----------------|
| Chromium | Chrome 98+ | Yes | Yes | Yes | `emoji-metadata`, `emoji-segmenter` |
| Firefox | Yes | Yes | Yes | Yes | U+FE0F detection + fallback |
| WebKit | Yes | Yes | Yes | Yes | CoreText color glyph + FreeType |
| Opera Presto | Unknown | Unknown | Unknown | Unknown | Platform fallback |

## Summary Table

| Aspect | Firefox | Chromium | WebKit | Opera Presto |
|--------|---------|----------|--------|--------------|
| FreeType scope | Linux/Android + PDFium/old OS | Universal (Win/Mac/Linux/Android) | Linux/WPE via `platform/graphics/freetype` | Likely Pango/FreeType on Linux |
| HarfBuzz version | Older | 14.2.1-22 (newer) | Via `platform/graphics/harfbuzz` | Proprietary shaping |
| HarfBuzz platform backends | Built and used | Intentionally excluded | Built as fallback when CoreText/AAT lacking | Not applicable |
| Font discovery | `gfx/thebes` platform lists + SharedFontList | `ui/gfx/font_list` + platform fallback files | CoreText font APIs + fontconfig | Proprietary font list |
| Color font support | COLRv0/v1, CBDT, SVG, SBIX | COLRv0/v1, CBDT, sbix, SVG; shipped COLRv1 in Chrome 98 | CoreText color glyph + FreeType path | Unknown |
| Emoji handling | U+FE0F detection, color-font preference | `emoji-metadata` + `emoji-segmenter`, color-font preference | CoreText color glyph + FreeType | Platform fallback |
| Rendering backend | Skia + WebRender | Skia + Skia Graphite | Skia / CoreText / Vulkan | Proprietary |
| SDF usage | No (browser text) | Limited (Chrome OS UI) | No browser text SDF found | No |
| Hinting control | `ShouldHintMetrics` + platform params | `FontRenderParams` with per-platform tuning | CoreText hinting + FreeType params | Platform hinting |
| Architecture | `gfx/thebes` abstraction layer | Blink (web) + `ui/gfx` (native) split | WebCore platform abstraction | Proprietary engine |
| Source files | `gfx/thebes/gfxFT2Fonts.cpp`, `gfx/thebes/COLRFonts.cpp` | `ui/gfx/font.h`, `third_party/blink/renderer/platform/fonts/` | `Source/WebCore/platform/graphics/{freetype,harfbuzz,coretext}/` | Not available |

## Implications for Vulkan Projects

From the three available source trees, the practical lessons are:

1. **Use FreeType as the universal rasterizer** — Chromium does this across all platforms; it avoids platform API fragmentation in a Vulkan app.

2. **Build HarfBuzz without platform backends** — Chromium's exclusion of `hb-coretext`, `hb-directwrite`, `hb-uniscribe` simplifies behavior and reduces surface area.

3. **Separate shaping from rasterization** — all three engines do this. HarfBuzz produces positioned glyphs; your renderer just draws quads from those positions.

4. **Color fonts need a separate path** — COLRv1 paint trees are vector operations; if you need them, borrow from WebKit's `platform/graphics/coretext` or Chromium's Skia Graphite path.

5. **Emoji = font fallback + color glyph detection** — detect U+FE0F and prefer color-font faces during fallback.
