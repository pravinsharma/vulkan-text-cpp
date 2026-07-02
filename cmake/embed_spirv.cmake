# Embed a SPIR-V binary as a C++ header with the data as a std::vector<uint32_t>.
#
# Usage:
#   cmake -DSPIRV_FILE=<path> -DOUTPUT_FILE=<path> -DVAR_NAME=<name> -P embed_spirv.cmake

if(NOT SPIRV_FILE OR NOT OUTPUT_FILE OR NOT VAR_NAME)
    message(FATAL_ERROR "embed_spirv.cmake: SPIRV_FILE, OUTPUT_FILE, and VAR_NAME are required")
endif()

file(READ "${SPIRV_FILE}" HEX_DATA HEX)

# CMake emits hex pairs without separators, so length is 2 chars per byte.
string(LENGTH "${HEX_DATA}" HEX_LEN)
math(EXPR BYTE_COUNT "${HEX_LEN} / 2")
math(EXPR PADDED_BYTES "(((${BYTE_COUNT}) + 3) / 4) * 4")

set(WORDS "")
set(OFFSET 0)
while(OFFSET LESS PADDED_BYTES)
    math(EXPR HEX_OFF "${OFFSET} * 2")
    string(SUBSTRING "${HEX_DATA}" ${HEX_OFF} 2 B0)
    math(EXPR HEX_OFF1 "${HEX_OFF} + 2")
    string(SUBSTRING "${HEX_DATA}" ${HEX_OFF1} 2 B1)
    math(EXPR HEX_OFF2 "${HEX_OFF} + 4")
    string(SUBSTRING "${HEX_DATA}" ${HEX_OFF2} 2 B2)
    math(EXPR HEX_OFF3 "${HEX_OFF} + 6")
    string(SUBSTRING "${HEX_DATA}" ${HEX_OFF3} 2 B3)
    if(OFFSET GREATER_EQUAL BYTE_COUNT)
        set(WORD "0x00000000u")
    else()
        set(WORD "0x${B3}${B2}${B1}${B0}u")
    endif()
    if(WORDS STREQUAL "")
        set(WORDS "${WORD}")
    else()
        set(WORDS "${WORDS}, ${WORD}")
    endif()
    math(EXPR OFFSET "${OFFSET} + 4")
endwhile()

file(WRITE "${OUTPUT_FILE}"
"#pragma once

#include <cstdint>
#include <vector>

inline std::vector<uint32_t> ${VAR_NAME} = {
${WORDS}
};
")
