# COLRv1 and Emoji Implementation Plan

## Current State

The `freetype-shaping` branch has:
- HarfBuzz shaping via `hb_shape()`
- FreeType rasterization into a grayscale atlas
- Monochrome bitmap text rendering

Color-font support is not implemented yet. This document describes how to add it.

## Objective

Add full COLRv1, COLRv0, CBDT/CBLC, and emoji support while keeping the existing HarfBuzz + FreeType + Vulkan stack. Do not introduce platform-native font APIs (DirectWrite/CoreText) and do not switch to MSDF for color fonts.

## Detection Layer

Add color-font detection before rendering any glyph.

### Per-face detection

In `TextRenderer::init`, after creating `FT_Face`, probe once:

```cpp
bool hasColor = false;
FT_ULong tableTag = FT_MAKE_TAG('C', 'O', 'L', 'R');
if (FT_Load_Sfnt_Table(ftFace_, tableTag, 0, nullptr, nullptr) == 0) hasColor = true;
tableTag = FT_MAKE_TAG('C', 'B', 'D', 'T');
if (!hasColor && FT_Load_Sfnt_Table(ftFace_, tableTag, 0, nullptr, nullptr) == 0) hasColor = true;
tableTag = FT_MAKE_TAG('S', 'V', 'G', ' ');
if (!hasColor && FT_Load_Sfnt_Table(ftFace_, tableTag, 0, nullptr, nullptr) == 0) hasColor = true;
```

Cache the result in `FT_Face` user data or a `std::unordered_map<FT_Face*, bool>`.

### Per-codepoint detection

Before shaping or during fallback, check whether a codepoint requests emoji presentation:

- If the text contains U+FE0F immediately after a base codepoint, treat it as emoji.
- Otherwise, use `hb_font_get_nominal_glyph()` to get the glyph index, then check whether the current `FT_Face` has a color glyph for that index.
- If not, continue fallback to other faces.

## Rendering Paths

### 1. COLRv0 and CBDT/CBLC via FreeType

FreeType 2.10+ already supports color-layered glyphs. Use this first because it requires no custom table parsing.

Load glyph with color flag:

```cpp
FT_Load_Glyph(ftFace_, glyphIndex, FT_LOAD_DEFAULT | FT_LOAD_COLOR);
```

Then inspect `FT_GlyphSlot`:

- If the glyph is a bitmap (`glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA`), upload the BGRA bitmap directly.
- If the glyph has layered paint data, iterate layers and composite them.
- For CBDT/CBLC embedded bitmaps, the bitmap mode is typically `FT_PIXEL_MODE_BGRA`.

Create a dedicated color atlas image or a separate RGBA atlas alongside the existing monochrome atlas.

### 2. COLRv1 Paint-Tree Interpreter

If FreeType’s built-in color path is insufficient for COLRv1, implement a minimal interpreter.

#### Parsing

Parse the `COLR` table paint records from the font file. Use FreeType’s `FT_OpenType_Table()` or your own SFNT parser to access raw table bytes.

Supported paint ops (start simple):

- `PaintColrLayers` — recurse into layer list
- `PaintSolid` — solid fill with palette index
- `PaintLinearGradient` — gradient fill
- `PaintTranslate`, `PaintScale`, `PaintRotate`, `PaintSkew` — affine transforms
- `PaintComposite` — porter-duff compositing
- `PaintGlyph` — recurse into another glyph
- `Clip` / `ClipBox` — clipping regions

Skip advanced ops initially: `VarColorStop*`, `VarTransform`, `PaintVar*,` `VarComposite`.

#### Execution

Interpret the paint tree into one of two backends:

**Option A: CPU rasterization into atlas**
- Rasterize each paint layer into a temporary RGBA buffer.
- Composite layers using porter-duff operators.
- Upload result to a Vulkan image.
- Suitable for complex COLRv1 glyphs without real-time animation.

**Option B: Vulkan draw-call decomposition**
- Map simple paint ops to instanced quads or triangle fans.
- Composite with a full-screen pass or per-glyph render pass.
- Better for animated or frequently updated glyphs at the cost of more pipeline complexity.

For the first implementation, use Option A.

#### Palette

Parse `CPAL` table. Store palette colors as `VkClearColorValue`-compatible arrays or a small SSBO. When painting, resolve palette indices to RGBA.

### 3. SVG-in-OpenType (later)

SVG table glyphs are vector drawings rendered via SVG. Options:

- Embed a minimal SVG path rasterizer.
- Convert SVG paths to triangle meshes and render through Vulkan.
- Use a compute shader to fill signed distance fields from SVG paths.

Do this last; it is the hardest path and rarely needed outside of emoji.

## Emoji Fallback Chain

Update `shapeText()` to preserve emoji presentation semantics:

1. Scan input UTF-8 for U+FE0F variation selector.
2. When found, mark the preceding base codepoint as `emojiPresentation = true`.
3. During glyph lookup, prefer a face that advertises color-glyph support for that codepoint.
4. If the primary face lacks the color glyph, fall back through the fallback chain to a face that has it.
5. If no color font covers the codepoint, render the monochrome glyph in the requested text color.

Pseudo-flow inside `shapeText`:

```cpp
std::vector<ShapedGlyph> out;
bool emoji = false;
for (glyph : harfbuzz_output) {
    if (glyph is U+FE0F) { emoji = true; continue; }
    if (emoji) { glyph.colorGlyph = findColorGlyphFace(glyph); }
    out.push_back(positionedGlyph);
    emoji = false;
}
```

## Atlas and Memory Layout

Keep the existing grayscale atlas for monochrome text. Add a separate RGBA atlas for color glyphs:

```cpp
VkImage colorAtlasImage_;
VkDeviceMemory colorAtlasMemory_;
VkImageView colorAtlasView_;
VkSampler colorAtlasSampler_;
std::vector<std::uint8_t> colorAtlasPixels_;
```

Why separate:
- Monochrome text benefits from `VK_FORMAT_R8_UNORM` and tight packing.
- Color glyphs need `VK_FORMAT_R8G8B8A8_UNORM` and may be larger due to embedded bitmaps or painted layers.
- Separate descriptor set or array index keeps shader branches simple.

## Shader Changes

Update `shaders/text.vert` and `shaders/text.frag`:

- Add a `colorGlyph` boolean in per-vertex data or push constants.
- For color glyphs, sample `VK_FORMAT_R8G8B8A8_UNORM` atlas.
- For COLRv1 CPU-composited layers, the atlas already contains final RGBA; just sample and output.
- If per-glyph palette tuning is needed, add a small palette SSBO or push constant with 1–2 palette entries.

## File Changes

### `src/TextRenderer.h`

- Add `bool hasColorGlyph(uint32_t glyphIndex, FT_Face* face) const;`
- Add `struct ColorGlyphInfo { uint32_t faceIndex; uint32_t glyphIndex; uint32_t paletteIndex; };`
- Add color atlas members (`colorAtlasImage_`, `colorAtlasView_`, `colorAtlasSampler_`, `colorAtlasPixels_`)
- Add `std::vector<ColorGlyphInfo> resolveColorGlyphs(const std::vector<ShapedGlyph>&) const;`
- Add `void createColorAtlasImage();`, `void uploadColorAtlasImage();`

### `src/TextRenderer.cpp`

- Add color-font detection in `init`
- Extend `shapeText()` to annotate `ShapedGlyph::colorGlyph`
- Add `drawColorGlyph` path that uploads CPU-composited RGBA layers into the color atlas
- Add `recreateColorAtlasIfNeeded()` if atlas grows

### `shaders/text.frag`

- Add sampler for color atlas or reuse existing atlas with format branching
- For color glyphs, discard smoothing logic and output sampled RGBA directly

### `src/Application.cpp`

- If dynamic palette switching is needed, expose palette colors through the renderer API

## Implementation Order

1. Detect color fonts and emoji selectors
2. Render COLRv0 / CBDT / CBLC bitmaps through FreeType’s color APIs into a separate RGBA atlas
3. Render those glyphs in text with the existing quad pipeline
4. Add a minimal COLRv1 paint-tree interpreter for the most common paint ops
5. Add CPAL palette resolution
6. Add SVG-in-OpenType only if required

## What to Avoid

- Do not use MSDF/MSDF for color fonts; COLRv1 is vector paint-based.
- Do not reintroduce platform APIs for color fonts; keep FreeType as the universal backend.
- Do not implement full SVG rasterization unless an actual font/feature needs it.
- Do not merge color-font code into the monochrome atlas; keep them separate until the design is stable.
