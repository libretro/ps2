#!/bin/sh
# Generate GameDatabaseBuiltin.cpp from bin/resources/GameIndex.yaml.
# POSIX shell + od + awk only -- no new build dependencies.
# Usage: gen_gamedb_builtin.sh <yaml> <out.cpp>
set -e
YAML="$1"; OUT="$2"
{
	printf '%s\n' \
'// Auto-generated from bin/resources/GameIndex.yaml. Do not edit by hand.' \
'// Generated at build time by tools/gen_gamedb_builtin.sh.' \
'//' \
'// Embeds the PCSX2 game-compatibility database into the core so it works' \
'// without an external <systemdir>/resources/GameIndex.yaml.' \
'' \
'#include <cstddef>' \
'' \
'extern const unsigned char g_gameDatabaseBuiltin[];' \
'extern const size_t g_gameDatabaseBuiltinSize;' \
'' \
'const unsigned char g_gameDatabaseBuiltin[] = {'
	od -An -v -t x1 "$YAML" | awk '
	{
		for (i = 1; i <= NF; i++) {
			line = line "0x" $i ","
			if (++n == 20) { print line; line = ""; n = 0 }
		}
	}
	END { if (line != "") print line }'
	printf '%s\n' '};' '' \
'const size_t g_gameDatabaseBuiltinSize = sizeof(g_gameDatabaseBuiltin);'
} > "$OUT"
