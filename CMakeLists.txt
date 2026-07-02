cmake_minimum_required(VERSION 3.20)

project(vulkan-text-cpp
    VERSION 0.1.0
    DESCRIPTION "Vulkan + GLFW application"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Require the Vulkan SDK. The vcpkg toolchain is selected via the preset
# (CMAKE_TOOLCHAIN_FILE), since it must be in place before this file runs.
if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR
        "VULKAN_SDK environment variable is not set. "
        "Install the Vulkan SDK and set VULKAN_SDK before configuring."
    )
endif()

find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(glfw3 CONFIG REQUIRED)

set(SOURCES
    src/main.cpp
    src/Application.h
    src/Application.cpp
    src/Renderer.h
    src/Renderer.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    third_party
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
)

# Compile GLSL shaders to SPIR-V with glslc.
set(SHADERS
    text.vert
    text.frag
)

set(SPIRV_OUTPUTS)
foreach(shader ${SHADERS})
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv)
    list(APPEND SPIRV_OUTPUTS ${spv})
    add_custom_command(
        OUTPUT ${spv}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
                -o ${spv}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
        COMMENT "Compiling ${shader} -> ${shader}.spv"
        VERBATIM
    )
endforeach()

add_custom_target(compile_shaders DEPENDS ${SPIRV_OUTPUTS})
add_dependencies(${PROJECT_NAME} compile_shaders)

# Pull GLFW3's runtime DLL next to the executable on Windows.
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:glfw>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
        COMMENT "Copying GLFW3.dll next to ${PROJECT_NAME}.exe"
    )
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    GLFW_INCLUDE_VULKAN
    NOMINMAX
)
