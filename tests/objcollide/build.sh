#!/bin/sh
# Object-path collisions in the Makefile.
#
# Makefile.common builds $(OBJECTS) by substituting the extension away:
# SOURCES_CXX:.cpp=.o, SOURCES_C:.c=.o, SOURCES_CC:.cc=.o, SOURCES_ASM:.S=.o.
# Two sources whose paths differ only in extension therefore name the SAME
# object, make builds it once, and the first pattern rule that matches wins
# -- %.o: %.cpp is listed first, so the C file is silently never compiled and
# its symbols come up undefined at link.
#
# A directory holding foo.c and foo.cpp must give them distinct basenames.
# This lists every object claimed by more than one source and, separately,
# every one claimed by sources in more than one language, which is the case
# that loses code rather than merely repeating a line.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

python3 - "$ROOT" <<'PY'
import collections, os, re, sys

root = sys.argv[1]
src  = open(os.path.join(root, 'Makefile.common')).read()

# Strip line continuations so one entry is one token, then take every path
# that ends in a source extension the object list substitutes away.
flat  = src.replace('\\\n', ' ')
claim = collections.defaultdict(set)
for m in re.finditer(r'(\$\(\w+\)/[\w./+-]+)\.(cpp|cc|c|S)\b', flat):
    claim[m.group(1) + '.o'].add((m.group(1) + '.' + m.group(2), m.group(2)))

dup  = {o: s for o, s in claim.items() if len(s) > 1}
lang = {o: s for o, s in dup.items() if len({e for _, e in s}) > 1}

for o, s in sorted(lang.items()):
    print('COLLISION %s <- %s' % (o, ', '.join(sorted(p for p, _ in s))))
for o, s in sorted(dup.items()):
    if o not in lang:
        print('repeated  %s <- %s' % (o, ', '.join(sorted(p for p, _ in s))))

if lang:
    print('FAIL: %d object(s) claimed by sources in two languages' % len(lang))
    sys.exit(1)
print('PASS: no object is claimed by sources in two languages'
      + (' (%d harmless repeat(s))' % len(dup) if dup else ''))
PY
