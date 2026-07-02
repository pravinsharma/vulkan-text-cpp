cmake_minimum_required(VERSION 3.20)

project(vulkan-text-cpp
    VERSION 0.1.0
    DESCRIPTION "Vulkan + GLFW + FreeType text rendering"
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

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(freetype CONFIG REQUIRED)

# Compile GLSL shaders to SPIR-V at configure/build time.
find_program(GLSLC glslc REQUIRED HINTS "$ENV{VULKAN_SDK}/Bin")

set(SHADER_SOURCES
    shaders/text.vert
    shaders/text.frag
    shaders/rect.frag
)

set(SHADER_SPV
    ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.vert.spv
    ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.frag.spv
    ${CMAKE_CURRENT_BINARY_DIR}/shaders/rect.frag.spv
)

set(SHADER_SPV_H
    ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.vert.spv.h
    ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.frag.spv.h
    ${CMAKE_CURRENT_BINARY_DIR}/shaders/rect.frag.spv.h
)

add_custom_command(
    OUTPUT ${SHADER_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
    COMMAND ${GLSLC} -fshader-stage=vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/text.vert
        -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.vert.spv
    COMMAND ${GLSLC} -fshader-stage=frag ${CMAKE_CURRENT_SOURCE_DIR}/shaders/text.frag
        -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.frag.spv
    COMMAND ${GLSLC} -fshader-stage=frag ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rect.frag
        -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/rect.frag.spv
    DEPENDS ${SHADER_SOURCES}
    COMMENT "Compiling GLSL shaders to SPIR-V"
    VERBATIM
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.vert.spv.h
    COMMAND ${CMAKE_COMMAND}
        -DSPIRV_FILE=${CMAKE_CURRENT_BINARY_DIR}/shaders/text.vert.spv
        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/shaders/text.vert.spv.h
        -DVAR_NAME=text_vert_spv
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.vert.spv
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Embedding text.vert.spv into header"
    VERBATIM
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.frag.spv.h
    COMMAND ${CMAKE_COMMAND}
        -DSPIRV_FILE=${CMAKE_CURRENT_BINARY_DIR}/shaders/text.frag.spv
        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/shaders/text.frag.spv.h
        -DVAR_NAME=text_frag_spv
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/shaders/text.frag.spv
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Embedding text.frag.spv into header"
    VERBATIM
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/rect.frag.spv.h
    COMMAND ${CMAKE_COMMAND}
        -DSPIRV_FILE=${CMAKE_CURRENT_BINARY_DIR}/shaders/rect.frag.spv
        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/shaders/rect.frag.spv.h
        -DVAR_NAME=rect_frag_spv
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/shaders/rect.frag.spv
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Embedding rect.frag.spv into header"
    VERBATIM
)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/Application.h
    src/Application.cpp
    src/TextRenderer.h
    src/TextRenderer.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/shaders
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
    freetype
)

set_source_files_properties(src/TextRenderer.cpp PROPERTIES
    OBJECT_DEPENDS "${SHADER_SPV_H}"
)

# Pull GLFW3's runtime DLL next to the executable on Windows.
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:glfw>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
        COMMENT "Copying GLFW3.dll next to ${PROJECT_NAME}.exe"
    )
    if(DEFINED ENV{VCPKG_ROOT})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$ENV{VCPKG_ROOT}/installed/x64-windows/bin/freetype.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
            COMMENT "Copying freetype.dll next to ${PROJECT_NAME}.exe"
        )
    endif()
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    GLFW_INCLUDE_VULKAN
)
