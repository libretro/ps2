# Generate GameDatabaseBuiltin.cpp from bin/resources/GameIndex.yaml.
#
# Script-mode CMake, so the only dependency is the CMake already running the
# build. The shell version this replaces needed sh + od + awk, which the MSVC
# Windows runner does not have ("'sh' is not recognized as an internal or
# external command").
#
# Usage: cmake -DYAML=<in> -DOUT=<out.cpp> -P gen_gamedb_builtin.cmake

if(NOT DEFINED YAML OR NOT DEFINED OUT)
	message(FATAL_ERROR "YAML and OUT must be set")
endif()

file(READ "${YAML}" hex HEX)
if(hex STREQUAL "")
	message(FATAL_ERROR "${YAML} is empty or unreadable")
endif()

# "0a1b..." -> "0x0a,0x1b,...", then a newline every 20 bytes so the generated
# file is not one multi-megabyte line.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
# CMake's regex has no {n} repetition, so the 20-byte row is spelled out.
set(row "")
foreach(i RANGE 1 20)
	string(APPEND row "0x..,")
endforeach()
string(REGEX REPLACE "(${row})" "\\1\n\t" body "${body}")

file(WRITE "${OUT}"
"// Auto-generated from bin/resources/GameIndex.yaml. Do not edit by hand.
// Generated at build time by tools/gen_gamedb_builtin.cmake.
//
// Embeds the PCSX2 game-compatibility database into the core so it works
// without an external <systemdir>/resources/GameIndex.yaml.

#include <cstddef>

extern const unsigned char g_gameDatabaseBuiltin[];
extern const size_t g_gameDatabaseBuiltinSize;

const unsigned char g_gameDatabaseBuiltin[] = {
\t${body}
};

const size_t g_gameDatabaseBuiltinSize = sizeof(g_gameDatabaseBuiltin);
")
