#!/usr/bin/env python3
"""Cross-architecture drift audit for the duplicated recompiler logic.

pcsx2/x86 and pcsx2/arm64 carry independent implementations of the same
analyses -- flag passes, branch setup, block bookkeeping, VIF block hashing.
Two bugs have already come out of that duplication (the x86 COP2 flag hack's
clobbered cpuRegs.code, and the arm64 flag hack's adjacency-instead-of-horizon
bracket), and in both cases the two sides disagreed in a way nothing tested.

This script pairs every function defined in both trees, normalises away the
differences that are known to be spelling rather than behaviour, and prints
what is left. It proves nothing on its own -- it is a reading list, ordered so
that the pairs most likely to be transcription drift come first.

Cosmetic normalisations applied (each one is a difference in how the two trees
are written, not in what they do):

  * mVU->field vs mVU.field, pState->x vs pState.x  (the x86 tree took
    pointers during the C89 conversion; arm64 still uses references)
  * brace style and blank lines
  * true/false vs 1/0             (x86 is C89-styled now)
  * C89_MIN/C89_MAX vs std::min/std::max
  * x86ptrStart vs codeStart      (field renamed on one side only)
  * mVUbm_add(m, ...) vs m->add(...)  (block manager de-classed on x86)

Usage:  python3 tests/analysis/xarch_drift.py [--full] [name ...]
        --full   print the diff for every differing pair, not just a summary
"""

import difflib
import glob
import re
import sys


def function_bodies(files):
    """Map function name -> (file, body-with-braces) for top-level definitions."""
    out = {}
    sig = re.compile(
        r"^(?:static\s+|__ri\s+|__fi\s+|extern\s+|inline\s+)*"
        r"[\w:\*&<>]+[\s\*]+(\w+)\s*\(([^;{]*)\)\s*(?:const\s*)?\{",
        re.M)
    for f in files:
        s = open(f, errors='ignore').read()
        for m in sig.finditer(s):
            name = m.group(1)
            k = s.index('{', m.start())
            depth = 0
            j = k
            while j < len(s):
                if s[j] == '{':
                    depth += 1
                elif s[j] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            out.setdefault(name, (f, s[k:j + 1]))
    return out


def normalise(body):
    """Strip comments and the known-cosmetic spelling differences."""
    b = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    b = re.sub(r"//[^\n]*", "", b)
    # pointer vs reference member access on the shared state objects
    b = re.sub(r"\b(mVU|pState|pinst|mFC|prog|block|v)->", r"\1.", b)
    # bool spelling
    b = re.sub(r"\btrue\b", "1", b)
    b = re.sub(r"\bfalse\b", "0", b)
    # min/max spelling
    b = re.sub(r"\bC89_MIN\b", "std::min", b)
    b = re.sub(r"\bC89_MAX\b", "std::max", b)
    b = re.sub(r"\bMIN_U32\b", "std::min", b)
    # renamed field
    b = re.sub(r"\bx86ptrStart\b", "codeStart", b)
    # block manager: free function vs method
    b = re.sub(r"mVUbm_(\w+)\(([^,]+),\s*", r"\2.\1(", b)
    # collapse whitespace and braces-on-their-own-line
    lines = []
    for l in b.split('\n'):
        l = l.strip()
        if not l or l in ('{', '}'):
            continue
        l = re.sub(r"\s+", " ", l)
        lines.append(l)
    return lines


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('-')]
    full = '--full' in sys.argv

    x = function_bodies(glob.glob('pcsx2/x86/**/*.cpp', recursive=True) +
                        glob.glob('pcsx2/x86/**/*.inl', recursive=True))
    a = function_bodies(glob.glob('pcsx2/arm64/*.cpp') +
                        glob.glob('pcsx2/arm64/*.inl'))

    shared = sorted(set(x) & set(a))
    if argv:
        shared = [n for n in shared if n in argv]

    identical, differing = [], []
    for n in shared:
        nx, na = normalise(x[n][1]), normalise(a[n][1])
        if nx == na:
            identical.append(n)
        else:
            ratio = difflib.SequenceMatcher(None, nx, na).quick_ratio()
            differing.append((ratio, n, nx, na))
    differing.sort(key=lambda t: -t[0])

    print("shared functions: %d   identical after normalisation: %d   differing: %d"
          % (len(shared), len(identical), len(differing)))
    print()
    print("Differing pairs, closest first -- the top of this list is where")
    print("transcription drift hides; the bottom is where the two ports are")
    print("legitimately different code.")
    print()
    for ratio, n, nx, na in differing:
        print("  %.3f  %-26s x86=%3d lines  arm64=%3d lines" % (ratio, n, len(nx), len(na)))
        if full or argv:
            for l in difflib.unified_diff(nx, na, lineterm='', n=0,
                                          fromfile='x86', tofile='arm64'):
                if l.startswith(('+', '-')) and not l.startswith(('+++', '---')):
                    print("        " + l[:120])
            print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
