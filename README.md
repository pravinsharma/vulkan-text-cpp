# vulkan-text-cpp

A C++20 Vulkan + GLFW application. Built with CMake + Ninja; dependencies managed
by [vcpkg](https://github.com/microsoft/vcpkg) (manifest mode); Vulkan headers and
loader come from the official [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/).

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
+-- src/
|   +-- main.cpp            # entry point
|   +-- Application.h
|   +-- Application.cpp     # GLFW window + Vulkan instance
+-- scripts/
    +-- build.ps1
    +-- run.ps1
    +-- clean.ps1
    +-- check-env.ps1
```

## Manual CMake invocation

If you prefer to drive CMake directly (and you have ninja on PATH):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```
