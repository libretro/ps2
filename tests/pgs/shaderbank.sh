#!/bin/sh
# Read the paraLLEl-GS shader bank, and identify a module from its source.
#
# The GLSL sources are not in this tree. shaders/slangmosh.hpp is a
# precompiled SPIR-V bank -- thirty modules in one uint32 array -- and it
# ships name-stripped, so nothing in the repository says what the GPU does
# with a given input. It is still readable: this splits the bank into one
# .spv per module and disassembles them when spirv-dis is installed.
#
# Modules are NOT named. slangmosh does not emit them in the order
# slangmosh_iface.hpp declares them, and the bank holds one more module than
# that header declares, so numbering them off the declaration list produces
# confident wrong answers -- it calls the module that is really
# sample_circuit "ui_frag[1][1]". Identify a module by recompiling a
# candidate source and matching the fingerprint instead:
#
#   ./shaderbank.sh /tmp/bank ../../pcsx2/GS/parallel-gs/gs/shaders
#
# The id bound is what pins it. It is the compiler's result-id count, it
# survives spirv-opt, and it is far too specific to collide by accident.
# sample_circuit.frag is the only source still in the tree and it lands on
# module 28 this way.
#
# What the shaders do with a degenerate Q was read out of the bank like
# this; the note at the head of pgs_vertex_kernels.h says what was found.
# Needs: apt install spirv-tools glslang-tools
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
BANK=$ROOT/pcsx2/GS/parallel-gs/gs/shaders/slangmosh.hpp
OUT=${1:-${TMPDIR:-/tmp}/pgs-shaders}
SRC=$2

[ -f "$BANK" ] || { echo "no shader bank at $BANK"; exit 1; }
mkdir -p "$OUT"

python3 - "$BANK" "$OUT" <<'PY'
import re, sys, os

bank, out = sys.argv[1], sys.argv[2]
src = open(bank).read()
m = re.search(r'spirv_bank\[\] =\s*\{(.*?)\n\};', src, re.S)
if not m:
    sys.exit("could not find spirv_bank[] in %s" % bank)
W = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{8})u', m.group(1))]

# Every module starts with the magic, but the magic can also appear inside
# another module's constant data, so each candidate has to look like a
# header: version 1.x, schema 0, a plausible id bound.
starts = []
for i, w in enumerate(W):
    if w != 0x07230203 or i + 5 > len(W):
        continue
    ver, bound, schema = W[i + 1], W[i + 3], W[i + 4]
    if (ver >> 16) & 0xff == 1 and schema == 0 and 0 < bound < 1000000:
        starts.append(i)
starts.append(len(W))

def entry_name(w):
    """OpEntryPoint's name, when the module kept it."""
    i = 5
    while i < len(w):
        op, ln = w[i] & 0xffff, w[i] >> 16
        if ln == 0:
            return ""
        if op == 15 and ln > 3:
            b = b''.join(w[j].to_bytes(4, 'little') for j in range(i + 3, i + ln))
            return b.split(b'\0')[0].decode('utf8', 'replace')
        i += ln
    return ""

print("%d modules" % (len(starts) - 1))
print("  %-3s %-9s %-9s %s" % ("idx", "bound", "bytes", "entry"))
for k in range(len(starts) - 1):
    mod = W[starts[k]:starts[k + 1]]
    path = os.path.join(out, "%02d.spv" % k)
    open(path, 'wb').write(b''.join(w.to_bytes(4, 'little') for w in mod))
    print("  %-3d %-9d %-9d %s" % (k, mod[3], len(mod) * 4, entry_name(mod) or "(stripped)"))
PY

if command -v spirv-dis >/dev/null 2>&1; then
	for f in "$OUT"/*.spv; do
		spirv-dis --no-header "$f" > "${f%.spv}.dis" 2>/dev/null || true
	done
	echo
	echo "disassembled to $OUT/*.dis"
else
	echo
	echo "spirv-dis not installed (apt install spirv-tools); .spv only"
fi

if [ -n "$SRC" ] && [ -d "$SRC" ]; then
	echo
	echo "=== recompiling $SRC and matching each against the bank ==="
	command -v glslangValidator >/dev/null 2>&1 ||
		{ echo "glslangValidator not installed (apt install glslang-tools)"; exit 1; }
	for s in "$SRC"/*.comp "$SRC"/*.frag "$SRC"/*.vert; do
		[ -f "$s" ] || continue
		b=$(basename "$s"); stage=comp
		case $s in *.frag) stage=frag ;; *.vert) stage=vert ;; esac
		if ! glslangValidator --target-env vulkan1.3 -S $stage -I"$SRC" \
		     -P'#extension GL_GOOGLE_include_directive : require' \
		     -DFEEDBACK_COLOR=0 -DFEEDBACK_DEPTH=0 \
		     -o "$OUT/re.spv" "$s" >"$OUT/re.log" 2>&1; then
			printf '  %-26s does not compile against these headers\n' "$b"
			continue
		fi
		command -v spirv-opt >/dev/null 2>&1 &&
			spirv-opt -O "$OUT/re.spv" -o "$OUT/re.spv" >/dev/null 2>&1
		python3 - "$OUT" "$b" <<'PY'
import sys, os, glob
out, name = sys.argv[1], sys.argv[2]
d = open(os.path.join(out, "re.spv"), "rb").read()
bound = int.from_bytes(d[12:16], "little")
hit = []
for f in sorted(glob.glob(os.path.join(out, "[0-9][0-9].spv"))):
    b = open(f, "rb").read()
    if int.from_bytes(b[12:16], "little") == bound:
        hit.append((os.path.basename(f)[:2], len(b)))
if hit:
    for idx, sz in hit:
        print("  %-26s bound %-7d -> module %s  (%d vs %d bytes)"
              % (name, bound, idx, len(d), sz))
else:
    print("  %-26s bound %-7d -> no module matches; this is not what shipped,"
          % (name, bound))
    print("  %-26s    or glslang/spirv-opt differ from slangmosh's" % "")
PY
	done
	rm -f "$OUT/re.spv" "$OUT/re.log"
fi

echo
echo "PASS: shader bank split"
