# Embeds a binary file into a C/C++ header as a byte array.
#
# DXC emits DXIL directly and has no equivalent of fxc's /Fh bytecode-header
# switch, so the post-processing pass writes the header CMake can compile
# instead. Usage:
#   cmake -DINPUT=... -DOUTPUT=... -DSYMBOL=... -P embed_binary.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "embed_binary: INPUT, OUTPUT and SYMBOL are required")
endif()

file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_length)
math(EXPR byte_count "${hex_length} / 2")

# Four bytes per line keeps the generated header readable without making it
# large enough to slow the compiler down.
set(body "")
set(column 0)
set(buffer "")

string(REGEX MATCHALL "[0-9a-f][0-9a-f]" pairs "${content}")
list(LENGTH pairs pair_count)
math(EXPR last_index "${pair_count} - 1")
foreach(index RANGE 0 ${last_index})
    list(GET pairs ${index} pair)
    string(APPEND buffer "0x${pair},")
    math(EXPR column "${column} + 1")
    if(column EQUAL 8)
        string(APPEND body "    ${buffer}\n")
        set(buffer "")
        set(column 0)
    endif()
endforeach()
if(NOT buffer STREQUAL "")
    string(APPEND body "    ${buffer}\n")
endif()

get_filename_component(input_name "${INPUT}" NAME)
file(WRITE "${OUTPUT}"
"// Generated from ${input_name} by cmake/embed_binary.cmake. Do not edit.
//
// The DXIL container is passed straight to D3D12 as D3D12_SHADER_BYTECODE,
// which accepts DXIL as well as DXBC.

#ifndef ${SYMBOL}_H
#define ${SYMBOL}_H

#include <cstdint>

static const unsigned char ${SYMBOL}[] = {
${body}};

static const std::uint32_t ${SYMBOL}_size = ${byte_count}u;

#endif  // ${SYMBOL}_H
")