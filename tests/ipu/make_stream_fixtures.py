#!/usr/bin/env python3
"""Turn MPEG-2 elementary streams into fixtures for slice_hash.cpp.

The IPU decodes the macroblock layer: given a slice's bits and the picture
parameters, it produces macroblocks. Everything above that layer -- sequence
header, picture header, slice header -- is the EE's job on real hardware and
is parsed here instead, so the harness gets what a game's BDEC setup would
hand the IPU.

The reference pixels come from decoding the same stream with ffmpeg. That
makes this an end-to-end check of the VLC tables, the dequantiser, the scan
order, DC prediction and the inverse DCT against an independent decoder --
not just a pin on what the tree happens to produce today.

Regenerate with:
    ./make_stream_fixtures.py
which needs ffmpeg on PATH and writes stream_fixtures.h next to itself.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Default matrices, used when the sequence header does not carry its own.
DEFAULT_INTRA = [
     8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
]
DEFAULT_NON_INTRA = [16] * 64


class Bits:
    """MSB-first bit reader over a bytes object."""

    def __init__(self, data, pos=0):
        self.d = data
        self.pos = pos          # in bits

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.d[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def peek(self, n):
        save = self.pos
        v = self.u(n)
        self.pos = save
        return v


def start_codes(data):
    """Every (offset, code) in the stream, offset pointing at the 00 00 01."""
    out = []
    i = 0
    while True:
        i = data.find(b"\x00\x00\x01", i)
        if i < 0 or i + 3 >= len(data):
            return out
        out.append((i, data[i + 3]))
        i += 3


def parse(data):
    """Pull out the picture parameters and where each slice's macroblocks start."""
    seq = {
        "intra_q": list(DEFAULT_INTRA),
        "non_intra_q": list(DEFAULT_NON_INTRA),
        "width": 0,
        "height": 0,
    }
    pic = {
        "intra_dc_precision": 0,
        "picture_structure": 3,
        "frame_pred_frame_dct": 1,
        "concealment_motion_vectors": 0,
        "q_scale_type": 0,
        "intra_vlc_format": 0,
        "alternate_scan": 0,
        "coding_type": 1,
    }
    slices = []

    for off, code in start_codes(data):
        b = Bits(data, (off + 4) * 8)

        if code == 0xB3:                      # sequence_header
            seq["width"] = b.u(12)
            seq["height"] = b.u(12)
            b.u(4 + 4 + 18 + 1 + 10 + 1)      # aspect..constrained
            if b.u(1):
                seq["intra_q"] = [b.u(8) for _ in range(64)]
            if b.u(1):
                seq["non_intra_q"] = [b.u(8) for _ in range(64)]

        elif code == 0xB5:                    # extension
            ident = b.u(4)
            if ident == 8:                    # picture_coding_extension
                b.u(16)                       # f_codes
                pic["intra_dc_precision"] = b.u(2)
                pic["picture_structure"] = b.u(2)
                b.u(1)                        # top_field_first
                pic["frame_pred_frame_dct"] = b.u(1)
                pic["concealment_motion_vectors"] = b.u(1)
                pic["q_scale_type"] = b.u(1)
                pic["intra_vlc_format"] = b.u(1)
                pic["alternate_scan"] = b.u(1)

        elif code == 0x00:                    # picture_header
            b.u(10)                           # temporal_reference
            pic["coding_type"] = b.u(3)

        elif 0x01 <= code <= 0xAF:            # slice
            qsc = b.u(5)
            if b.peek(1):                     # intra_slice_flag
                b.u(1)
                b.u(1)                        # intra_slice
                b.u(7)                        # reserved
            while b.u(1):                     # extra_bit_slice
                b.u(8)
            slices.append({
                "row": code - 1,              # slice_vertical_position - 1
                "qsc": qsc,
                "bitpos": b.pos,              # first macroblock_address_increment
            })

    return seq, pic, slices


def emit(fh, name, seq, pic, slices, data, yuv):
    w, h = seq["width"], seq["height"]
    mbw = (w + 15) // 16
    fh.write("/* ---- %s: %dx%d, %d slices ---- */\n" % (name, w, h, len(slices)))
    fh.write("static const u8 %s_bytes[] = {\n" % name)
    for i in range(0, len(data), 16):
        fh.write("\t" + "".join("0x%02x," % c for c in data[i:i + 16]) + "\n")
    fh.write("};\n")

    fh.write("static const u8 %s_intra_q[64] = {%s};\n"
             % (name, ",".join(str(v) for v in seq["intra_q"])))
    fh.write("static const u8 %s_non_intra_q[64] = {%s};\n"
             % (name, ",".join(str(v) for v in seq["non_intra_q"])))

    # Reference macroblocks, straight out of ffmpeg's decode of the frame.
    ysz = w * h
    csz = (w // 2) * (h // 2)
    fh.write("static const u8 %s_ref[%d][384] = {\n" % (name, len(slices) * mbw))
    for s in slices:
        for mx in range(mbw):
            vals = []
            for yy in range(16):
                for xx in range(16):
                    vals.append(yuv[(s["row"] * 16 + yy) * w + mx * 16 + xx])
            for plane in (0, 1):
                base = ysz + plane * csz
                for yy in range(8):
                    for xx in range(8):
                        vals.append(yuv[base + (s["row"] * 8 + yy) * (w // 2)
                                        + mx * 8 + xx])
            fh.write("\t{" + ",".join(str(v) for v in vals) + "},\n")
    fh.write("};\n")

    fh.write("static const ipu_slice %s_slices[] = {\n" % name)
    for s in slices:
        fh.write("\t{ %d, %d, %d },\n" % (s["bitpos"], s["qsc"], s["row"]))
    fh.write("};\n")

    fh.write("""static const ipu_stream %s_stream = {
\t"%s", %s_bytes, sizeof(%s_bytes), %s_slices,
\t(int)(sizeof(%s_slices)/sizeof(%s_slices[0])),
\t%s_intra_q, %s_non_intra_q, (const u8 *)%s_ref,
\t%d, %d, %d, %d, %d, %d, %d, %d, %d
};

""" % (name, name, name, name, name, name, name, name, name, name,
       mbw, w, h,
       pic["intra_dc_precision"], pic["picture_structure"],
       pic["frame_pred_frame_dct"], pic["q_scale_type"],
       pic["intra_vlc_format"], pic["alternate_scan"]))


def build(spec, out):
    name, src, q, extra = spec
    m2v = os.path.join(out, name + ".m2v")
    raw = os.path.join(out, name + ".yuv")
    subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", src,
                    "-c:v", "mpeg2video", "-g", "1", "-bf", "0",
                    "-qscale:v", str(q), "-frames:v", "1"] + extra +
                   ["-f", "mpeg2video", m2v, "-y"], check=True)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-i", m2v,
                    "-pix_fmt", "yuv420p", "-f", "rawvideo", raw, "-y"],
                   check=True)
    return open(m2v, "rb").read(), open(raw, "rb").read()


def main():
    # Different qscale, different source material and both scan orders, so the
    # VLC tables are walked over a wide spread of run/level pairs rather than
    # whatever one clip happens to contain.
    specs = [
        ("bars",   "smptebars=size=64x64:rate=1:duration=1",   2,  []),
        ("test_q3", "testsrc2=size=64x64:rate=1:duration=1",   3,  []),
        ("test_q12", "testsrc2=size=64x64:rate=1:duration=1", 12,  []),
        ("noise",  "testsrc2=size=128x64:rate=1:duration=1",   6,  []),
        ("flat_q31", "color=c=gray:size=64x64:rate=1:duration=1", 31, []),
        ("smooth", "gradients=size=64x64:rate=1:duration=1",     8,  []),
    ]

    out = os.path.join(HERE, "_streams")
    os.makedirs(out, exist_ok=True)

    path = os.path.join(HERE, "stream_fixtures.h")
    with open(path, "w") as fh:
        fh.write("/* Generated by make_stream_fixtures.py -- do not edit.\n"
                 " *\n"
                 " * MPEG-2 slices from ffmpeg, with ffmpeg's own decode of the\n"
                 " * same frame as the reference pixels. */\n"
                 "#pragma once\n\n"
                 "typedef struct ipu_slice {\n"
                 "\tint bitpos;   /* first macroblock_address_increment */\n"
                 "\tint qsc;      /* quantiser_scale_code from the slice header */\n"
                 "\tint row;      /* macroblock row this slice covers */\n"
                 "} ipu_slice;\n\n"
                 "typedef struct ipu_stream {\n"
                 "\tconst char *name;\n"
                 "\tconst u8 *bytes;\n"
                 "\tsize_t nbytes;\n"
                 "\tconst ipu_slice *slices;\n"
                 "\tint nslices;\n"
                 "\tconst u8 *intra_q;\n"
                 "\tconst u8 *non_intra_q;\n"
                 "\tconst u8 *ref;        /* [nslices * mbw][384] */\n"
                 "\tint mbw, width, height;\n"
                 "\tint intra_dc_precision, picture_structure;\n"
                 "\tint frame_pred_frame_dct, q_scale_type;\n"
                 "\tint intra_vlc_format, alternate_scan;\n"
                 "} ipu_stream;\n\n")

        names = []
        for spec in specs:
            data, yuv = build(spec, out)
            seq, pic, slices = parse(data)
            if not slices:
                sys.exit("no slices found in %s" % spec[0])
            emit(fh, spec[0], seq, pic, slices, data, yuv)
            names.append(spec[0])
            print("%-10s %dx%d  %d slices  alt_scan=%d ivf=%d dcp=%d"
                  % (spec[0], seq["width"], seq["height"], len(slices),
                     pic["alternate_scan"], pic["intra_vlc_format"],
                     pic["intra_dc_precision"]))

        fh.write("static const ipu_stream * const ipu_streams[] = {\n")
        for n in names:
            fh.write("\t&%s_stream,\n" % n)
        fh.write("};\n")

    print("wrote %s" % path)


if __name__ == "__main__":
    main()
