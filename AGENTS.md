# AGENTS

Guidance for AI agents (and humans) working in this repository.

## Project at a glance

- **Language:** C++20
- **Build system:** CMake 3.20+ with Ninja
- **Windowing:** GLFW 3 (via vcpkg manifest)
- **Graphics:** Vulkan (headers + loader from `VULKAN_SDK`)
- **Text:** vendored `stb_truetype.h` (single header, MIT)
- **Platform:** Windows (MSVC, PowerShell 7)

## Required environment

| Variable       | Purpose                                          |
| -------------- | ------------------------------------------------ |
| `VULKAN_SDK`   | Vulkan headers, loader, and `glslc`              |
| `VCPKG_ROOT`   | Auto-loaded as CMake toolchain; supplies `glfw3` |

`scripts/check-env.ps1` prints which of these are set and whether `cmake`,
`ninja`, and `cl` are on `PATH`. The `build.ps1` / `run.ps1` scripts
prepend `E:\dev\bin` to `PATH` so `ninja` resolves even if the user has not
put it there themselves.

## Build / run commands

| Task                      | Command                                       |
| ------------------------- | --------------------------------------------- |
| Configure + build (Debug) | `pwsh ./scripts/build.ps1`                    |
| Build (Release)           | `pwsh ./scripts/build.ps1 -Preset release`    |
| Configure only            | `pwsh ./scripts/build.ps1 -ConfigureOnly`     |
| Build + run               | `pwsh ./scripts/build.ps1 -Run`               |
| Run (after build)         | `pwsh ./scripts/run.ps1`                      |
| Clean                     | `pwsh ./scripts/clean.ps1`                    |
| Verify env                | `pwsh ./scripts/check-env.ps1`                |

Build output goes to `build/` (Debug) or `build-release/` (Release). The
`glfw3.dll` is copied next to the executable by a CMake post-build step;
the compiled GLSL shaders are placed in `build/shaders/`.

## File layout (and what to edit)

- `CMakeLists.txt` — add new source files here. New libraries to link go
  in `target_link_libraries`. The vcpkg toolchain is selected via the
  preset (`CMAKE_TOOLCHAIN_FILE`); the build hard-fails if `VULKAN_SDK`
  is not set. `Vulkan_GLSLC_EXECUTABLE` is used to compile GLSL shaders
  to SPIR-V; `glslc` is requested as a `COMPONENTS` of `find_package`.
- `vcpkg.json` — add vcpkg ports here. Currently only `glfw3`. Do **not**
  add `vulkan` / `vulkan-headers`; those come from `VULKAN_SDK`.
- `CMakePresets.json` — `debug` and `release` presets, both Ninja.
  Each preset prepends `E:/dev/bin` to `PATH` so `ninja` is found and
  sets `CMAKE_TOOLCHAIN_FILE` to `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`.
- `third_party/stb_truetype.h` — vendored; updated by re-running the
  `Invoke-WebRequest` in the README. To upgrade, bump the URL.
- `shaders/text.vert` / `shaders/text.frag` — GLSL sources. Compiled to
  SPIR-V by a CMake custom command; outputs land in `<binary>/shaders/`.
  To add a new shader, add its name to the `SHADERS` list in
  `CMakeLists.txt` and ship the `.spv` via `<exe-dir>/shaders/`.
- `src/main.cpp` — entry point, exception-to-stderr shim.
- `src/Application.h` / `src/Application.cpp` — GLFW window + Vulkan
  instance + surface; owns the `Renderer`.
- `src/Renderer.h` / `src/Renderer.cpp` — owns the device, swapchain,
  render pass, pipeline, font atlas, sync objects. The text string
  lives in `Renderer::text_`. The font is loaded from the `kFontCandidates`
  list in `Renderer.cpp` (Segoe UI, Arial, Calibri, Consolas, Courier
  New, Georgia, Tahoma, Verdana — only the ones that exist on disk are
  kept). The active font drives the glyph atlas and the dropdown in the
  appbar.
- `scripts/*.ps1` — the user-facing build pipeline.

## Conventions

- C++20; do not enable C++ extensions (`-std=c++20`, not `-std=c++20gnu++20`).
- Headers use `#pragma once`.
- Use the `glfw` imported target (vcpkg) and `Vulkan::Vulkan`
  (CMake's `FindVulkan`).
- The `GLFW_INCLUDE_VULKAN` compile definition is set in `CMakeLists.txt`,
  so do not include `<vulkan/vulkan.h>` via GLFW; include it directly.
- `NOMINMAX` is set globally; do not rely on `min` / `max` macros from
  Windows headers — use `std::min` / `std::max`.
- Resource paths (shaders) are resolved relative to the executable
  directory at runtime (see `Renderer::exeDir`).
- Do **not** add code comments unless the user asks.
- Do not commit secrets, `build/`, or `vcpkg_installed/` artifacts.
- Do not commit unless the user explicitly asks.

## Common follow-up tasks

- **Add a new source file:** append it to the `add_executable` source list
  in `CMakeLists.txt`.
- **Add a dependency:** add the port to `vcpkg.json` (vcpkg manifest mode
  is already active). Rerun the build; vcpkg will install on the next
  configure.
- **Add a new shader:** drop the `.vert` / `.frag` under `shaders/`,
  add the basename to the `SHADERS` list in `CMakeLists.txt`, and load
  the resulting `.spv` in `Renderer::createPipeline`.
- **Change the text:** edit `Renderer::text_` in `src/Renderer.h`.
- **Change the font / size:** edit `kFontCandidates` and `kFontSize` in
  `src/Renderer.h`; the atlas is re-baked on init.
- **Add / remove a font in the dropdown:** edit `kFontCandidates` in
  `src/Renderer.cpp` (the list is filtered to entries that exist on
  disk at startup).
- **Tweak the appbar / dropdown layout:** the constants live on
  `Renderer` (`kAppbarHeight`, `kDropdownWidth`, `kDropdownHeight`,
  `kDropdownPadX`) and the colours are the `kColor*` arrays near the
  top of `Renderer.cpp`.
- **Add a Vulkan debug messenger:** extend `Application::createInstance`
  and add the `VK_EXT_debug_utils` extension in
  `Application::getRequiredExtensions`.
- **Multi-frame in flight / depth buffer / MSAA:** natural extension
  point is `Renderer::drawFrame`; see the "Things that have bitten us"
  section for the per-image-signal-semaphore pattern that is already
  in use.

## Things that have bitten us before

- Forgetting to set `VULKAN_SDK` — `CMakeLists.txt` will abort with a
  `FATAL_ERROR`; this is intentional.
- Linking against `glfw3dll` directly on Windows — the `glfw` imported
  target from vcpkg already brings in the right import lib; just use
  `target_link_libraries(... glfw)`.
- Forgetting that vcpkg shared builds need the DLL at runtime — handled
  by the post-build copy in `CMakeLists.txt`.
- `set(CMAKE_TOOLCHAIN_FILE ...)` in `CMakeLists.txt` is a no-op — the
  toolchain file loads before the project file. Set it via the preset
  (or on the cmake command line).
- `FindVulkan` variable is `Vulkan_GLSLC_EXECUTABLE` (no underscore
  between `GLSL` and `C`); the `glslc` component must be requested via
  `find_package(Vulkan REQUIRED COMPONENTS glslc)`.
- Windows headers define `min` / `max` as macros and break
  `std::numeric_limits<T>::max()`. We define `NOMINMAX` globally.
- Shader `.spv` files must be loaded relative to the executable
  directory, not the current working directory.
- Single render-finished semaphore across swapchain images triggers
  `VUID-vkQueueSubmit-pSignalSemaphores-00067` — the renderer keeps a
  `std::vector<VkSemaphore>` of per-image signal semaphores.
- Destroying the atlas (image/view/sampler) while the GPU still has
  pending reads of it trips the same family of validation errors as the
  semaphore case above. `Renderer::rebuildAtlasForFont` starts with
  `vkDeviceWaitIdle` and the inner transitions wait on
  `vkQueueWaitIdle` to be safe; the descriptor set is rewritten via
  `vkUpdateDescriptorSets` after the new image is uploaded.
