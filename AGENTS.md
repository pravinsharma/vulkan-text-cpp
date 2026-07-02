# AGENTS

Guidance for AI agents (and humans) working in this repository.

## Project at a glance

- **Language:** C++20
- **Build system:** CMake 3.20+ with Ninja
- **Windowing:** GLFW 3 (via vcpkg manifest)
- **Graphics:** Vulkan (headers + loader from `VULKAN_SDK`)
- **Platform:** Windows (MSVC, PowerShell 7)

## Required environment

| Variable       | Purpose                                          |
| -------------- | ------------------------------------------------ |
| `VULKAN_SDK`   | Vulkan headers, loader (`vulkan-1.lib`)          |
| `VCPKG_ROOT`   | Auto-loaded as CMake toolchain; supplies `glfw3` |

`scripts/check-env.ps1` prints which of these are set and whether `cmake`,
`ninja`, and `cl` are on `PATH`. The `build.ps1` / `run.ps1` scripts
prepend `E:\dev\bin` to `PATH` so `ninja` resolves even if the user has not
put it there themselves.

## Build / run commands

| Task                      | Command                                |
| ------------------------- | -------------------------------------- |
| Configure + build (Debug) | `pwsh ./scripts/build.ps1`             |
| Build (Release)           | `pwsh ./scripts/build.ps1 -Preset release` |
| Configure only            | `pwsh ./scripts/build.ps1 -ConfigureOnly` |
| Build + run               | `pwsh ./scripts/build.ps1 -Run`        |
| Run (after build)         | `pwsh ./scripts/run.ps1`               |
| Clean                     | `pwsh ./scripts/clean.ps1`             |
| Verify env                | `pwsh ./scripts/check-env.ps1`         |

Build output goes to `build/` (Debug) or `build-release/` (Release). The
`glfw3.dll` is copied next to the executable by a CMake post-build step.

## File layout (and what to edit)

- `CMakeLists.txt` — add new source files here (currently `src/main.cpp`,
  `src/Application.h`, `src/Application.cpp`). New libraries to link go
  in `target_link_libraries`. The vcpkg toolchain is auto-selected when
  `VCPKG_ROOT` is set; the build will hard-fail if `VULKAN_SDK` is not set.
- `vcpkg.json` — add vcpkg ports here. Currently only `glfw3`. Do **not**
  add `vulkan` / `vulkan-headers`; those come from `VULKAN_SDK`.
- `CMakePresets.json` — `debug` and `release` presets, both Ninja.
  Each preset prepends `E:/dev/bin` to `PATH` so `ninja` is found.
- `src/main.cpp` — entry point, exception-to-stderr shim.
- `src/Application.h` / `src/Application.cpp` — GLFW window + Vulkan
  instance creation. `framebufferResized_` is wired up for swapchain
  recreation (not implemented yet — that is the natural next step).
- `scripts/*.ps1` — the user-facing build pipeline.

## Conventions

- C++20; do not enable C++ extensions (`-std=c++20`, not `-std=c++20gnu++20`).
- Headers use `#pragma once`.
- Use the `glfw` imported target (vcpkg) and `Vulkan::Vulkan`
  (CMake's `FindVulkan`).
- The `GLFW_INCLUDE_VULKAN` compile definition is set in `CMakeLists.txt`,
  so do not include `<vulkan/vulkan.h>` via GLFW; include it directly.
- Do **not** add code comments unless the user asks.
- Do not commit secrets, `build/`, or `vcpkg_installed/` artifacts.
- Do not commit unless the user explicitly asks.

## Common follow-up tasks

- **Add a new source file:** append it to the `add_executable` source list
  in `CMakeLists.txt`.
- **Add a dependency:** add the port to `vcpkg.json` (vcpkg manifest mode
  is already active). Rerun the build; vcpkg will install on the next
  configure.
- **Add a Vulkan validation layer / debug messenger:** extend
  `Application::createInstance` and add the `VK_EXT_debug_utils` extension
  in `Application::getRequiredExtensions`.
- **Add swapchain / render pass:** the natural extension point is a new
  class (e.g. `Renderer`) owned by `Application`, plus a swapchain-recreate
  path driven by `framebufferResized_`.

## Things that have bitten us before

- Forgetting to set `VULKAN_SDK` — `CMakeLists.txt` will abort with a
  `FATAL_ERROR`; this is intentional.
- Linking against `glfw3dll` directly on Windows — the `glfw` imported
  target from vcpkg already brings in the right import lib; just use
  `target_link_libraries(... glfw)`.
- Forgetting that vcpkg shared builds need the DLL at runtime — handled
  by the post-build copy in `CMakeLists.txt`.
