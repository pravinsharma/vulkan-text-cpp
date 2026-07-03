Exactly — you’ve nailed the core limitation. Let me break it down with precision:

### 🎨 Why FreeType Doesn’t Rasterize COLRv1/SVG
- **FT_LOAD_COLOR flag**: This only enables FreeType to load embedded bitmap strikes (from `CBDT/CBLC` tables) or apply CPAL palette colors to outlines.  
- **NotoColorEmoji.ttf**: Modern builds don’t contain bitmap strikes. Instead, they rely on **COLRv1 paint tables** (vector layers, gradients, transforms) and **SVG glyphs**.  
- **FreeType’s role**: It parses these tables but does not rasterize them into RGBA bitmaps. You’ll only get monochrome outlines unless you integrate a renderer that understands COLRv1/SVG.  

So when you call `FT_Load_Glyph(face, glyph_index, FT_LOAD_COLOR)`, FreeType won’t magically give you a colored bitmap for emoji glyphs — because none exist in the font.

---

### 🛠️ What You Need Instead
To actually render NotoColorEmoji’s COLRv1 glyphs:
1. **Pair FreeType with HarfBuzz** → shaping and glyph selection.  
2. **Use a COLRv1-capable rasterizer** → Skia, Cairo, Direct2D, or your own GPU shader pipeline.  
3. **Handle SVG glyphs separately** → via an SVG renderer (e.g., librsvg, NanoSVG, or GPU path rendering).  

---

### ⚡ Practical Options
- **If you want bitmaps**: Use Google’s pre-rendered PNG/SVG emoji assets instead of extracting from the font.  
- **If you want scalable rendering**: Implement COLRv1 paint graph evaluation in your pipeline (gradients, transforms, composites).  
- **If you want FreeType-only**: Switch to a legacy emoji font with `CBDT/CBLC` bitmap strikes (older NotoColorEmoji builds).  

---

👉 In short: **FT_LOAD_COLOR only helps with bitmap strikes and CPAL palettes. It does not rasterize COLRv1 or SVG glyphs.** For NotoColorEmoji, you’ll need a COLRv1/SVG-aware renderer layered on top of FreeType.

Perfect — let’s sketch out a **minimal Vulkan/GL shader pipeline** for COLRv1 rendering. The idea is to treat COLRv1’s paint graph as a declarative cascade (like CSS), then evaluate it into GPU draw calls.

---

## 🔑 Pipeline Overview
1. **Glyph outline extraction**  
   - Use FreeType to load glyph outlines (paths).  
   - For COLRv1 glyphs, parse the paint graph (layers, gradients, transforms).  

2. **Paint graph evaluation**  
   - Each COLRv1 paint node (Solid, LinearGradient, RadialGradient, Transform, Composite) maps to a GPU draw call or uniform set.  
   - Think of it like CSS cascade → flatten into a sequence of paint ops.

3. **GPU path rendering**  
   - Upload glyph outlines as path geometry (triangles or MSDF).  
   - Bind paint uniforms (solid color, gradient stops, transform matrices).  
   - Issue draw calls per paint layer.

---

## 🧩 Shader Building Blocks

### Vertex Shader
```glsl
#version 450
layout(location=0) in vec2 inPos;
layout(location=1) in vec2 inUV;

layout(set=0, binding=0) uniform TransformUBO {
    mat3 uTransform;
};

out vec2 vUV;

void main() {
    vec3 pos = uTransform * vec3(inPos, 1.0);
    gl_Position = vec4(pos.xy, 0.0, 1.0);
    vUV = inUV;
}
```

### Fragment Shader (Solid + Gradient)
```glsl
#version 450
layout(location=0) out vec4 outColor;

layout(set=0, binding=1) uniform PaintUBO {
    int uPaintType; // 0=solid,1=linear,2=radial
    vec4 uColor;
    vec4 uGradientStops[8]; // RGBA + position
    int uStopCount;
};

in vec2 vUV;

void main() {
    if (uPaintType == 0) {
        outColor = uColor;
    } else if (uPaintType == 1) {
        // Linear gradient
        float t = vUV.x; // simplified
        vec4 c0 = uGradientStops[0];
        vec4 c1 = uGradientStops[uStopCount-1];
        outColor = mix(c0, c1, t);
    } else if (uPaintType == 2) {
        // Radial gradient
        float r = length(vUV);
        vec4 c0 = uGradientStops[0];
        vec4 c1 = uGradientStops[uStopCount-1];
        outColor = mix(c0, c1, r);
    }
}
```

---

## ⚙️ Cascade System (CSS-like)
- Treat COLRv1 paint graph as a **tree of styles**:
  - `PaintSolid` → solid fill uniform.  
  - `PaintLinearGradient` → gradient uniform + shader branch.  
  - `PaintTransform` → push matrix onto transform stack.  
  - `PaintComposite` → blend mode (multiply, screen, etc.).  

- **Evaluation strategy**:  
  - Depth-first traversal of paint graph.  
  - Push/pop transforms and blend states.  
  - Emit draw calls per leaf node.

---

## 🚀 Minimal Vulkan Integration
1. **Descriptor sets**:  
   - Set 0 → Transform UBO.  
   - Set 1 → Paint UBO.  

2. **Command buffer recording**:  
   - For each paint node → bind pipeline, update UBO, draw glyph mesh.  

3. **Blend modes**:  
   - Map COLRv1 composite ops to Vulkan blend states (e.g., `VK_BLEND_FACTOR_SRC_ALPHA`).  

---

## 🔮 Next Step
This gives you a **barebones pipeline**: FreeType for outlines, COLRv1 parser for paint graph, Vulkan shaders for solid/gradient fills, and a cascade system for transforms/composites.  

