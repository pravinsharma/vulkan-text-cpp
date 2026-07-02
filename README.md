# vulkan-text-cpp

A C++20 Vulkan + GLFW application that renders the text
**"A quick brown fox jumped over a lazy dog!"** in the center of the
window using [stb_truetype](https://github.com/nothings/stb) to bake a
glyph atlas and a hand-written GLSL pipeline to draw the quads.

Built with CMake + Ninja; dependencies managed by
[vcpkg](https://github.com/microsoft/vcpkg) (manifest mode); Vulkan
headers, loader, and `glslc` come from the official
[Vulkan SDK](https://www.lunarg.com/vulkan-sdk/).

## Prerequisites

| Tool / SDK         | Notes                                                                  |
| ------------------ | ---------------------------------------------------------------------- |
| MSVC (cl.exe)      | C++20 support required                                                 |
| CMake              | 3.20 or newer (3.25+ recommended for presets)                          |
| Ninja              | Expected at `E:\dev\bin\ninja.exe` (auto-added to PATH by the scripts) |
| Vulkan SDK         | Set the `VULKAN_SDK` environment variable to its install root          |
| vcpkg              | Set the `VCPKG_ROOT` environment variable to the vcpkg checkout        |

Verify the environment:

```powershell
pwsh ./scripts/check-env.ps1
```

## Build

Debug build (default):

```powershell
pwsh ./scripts/build.ps1
```

Release build:

```powershell
pwsh ./scripts/build.ps1 -Preset release
```

Configure only (no compile):

```powershell
pwsh ./scripts/build.ps1 -ConfigureOnly
```

Build and immediately run:

```powershell
pwsh ./scripts/build.ps1 -Run
```

## Run

```powershell
pwsh ./scripts/run.ps1
pwsh ./scripts/run.ps1 -Preset release
```

## Clean

```powershell
pwsh ./scripts/clean.ps1
```

## Layout

```
.
+-- CMakeLists.txt          # build configuration
+-- CMakePresets.json       # debug / release presets (Ninja)
+-- vcpkg.json              # manifest deps (glfw3)
+-- third_party/
|   +-- stb_truetype.h      # vendored, MIT
+-- shaders/
|   +-- text.vert           # GLSL vertex shader
|   +-- text.frag           # GLSL fragment shader
+-- src/
    +-- main.cpp            # entry point
    +-- Application.h
    +-- Application.cpp     # GLFW window + Vulkan instance
    +-- Renderer.h
    +-- Renderer.cpp        # swapchain, pipeline, font atlas, text draw
+-- scripts/
    +-- build.ps1
    +-- run.ps1
    +-- clean.ps1
    +-- check-env.ps1
```

## How text rendering works

1. On init, `Renderer` reads a Windows system font (Segoe UI, then Arial,
   then Segoe UI Semibold) into a CPU buffer.
2. `stbtt_BakeFontBitmap` bakes glyphs `!`..`~` (ASCII 33..126) into a
   single-channel 512x512 atlas; the resulting `stbtt_bakedchar` table is
   kept for layout.
3. The atlas is uploaded into a `VK_FORMAT_R8_UNORM` image via a staging
   buffer, with the proper `UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY`
   layout transitions.
4. Every frame, `Renderer::buildTextVertices` walks the text with
   `stbtt_GetBakedQuad` to produce two triangles per character (position
   + UV) and offsets the whole string so its measured bounding box sits in
   the middle of the swapchain extent.
5. The orthographic projection (push constant, 64 bytes) maps pixel space
   into Vulkan's NDC. The fragment shader samples the R channel of the
   atlas and writes white with that as alpha, so the result blends cleanly
   over a dark background.

## Customising

- **Text content** — edit `Renderer::text_` in `src/Renderer.h`.
- **Font / size** — edit `kFontCandidatePaths` and `kFontSize` in
  `src/Renderer.h`; the atlas is re-baked on init.
- **Text colour** — change the `vec4(1.0)` in `shaders/text.frag`.
- **Background colour** — change the clear value in
  `Renderer::recordCommandBuffer` (`VkClearValue`).

## Manual CMake invocation

If you prefer to drive CMake directly (and you have ninja on PATH):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```
