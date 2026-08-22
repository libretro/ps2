#!/usr/bin/env python3
"""Wrap a disc image in a CISO v1 container, for testing CsoFileReader.

The compressed CDVD readers had no test coverage at all: a .cue exercises
FlatFileReader and nothing else, so any change to CsoFileReader,
GzippedFileReader or ChdFileReader was unverifiable. This produces a real
.cso from an existing image, which turns the CSO path into a gate:

    python3 tests/cdvd/make_cso.py game.bin game.cso

Then run the emulator against both and compare. The two must produce
identical output -- the compressed reader has to hand back exactly the bytes
the flat reader does -- which is a stronger check than either run alone:

    PCSX2_FBHASH=1 headless core game.cue  1200 sys | grep FBHASH > flat.txt
    PCSX2_FBHASH=1 headless core game.cso  1200 sys | grep FBHASH > cso.txt
    diff flat.txt cso.txt

Format, from CsoFileReader.cpp: a 24-byte header, then (frames + 1) 32-bit
index entries, then the frame payloads. An index entry is the file offset of
its frame; the top bit means the frame is stored uncompressed. A frame's
compressed length is the difference between consecutive index entries, which
is why the index has one extra terminating entry. Payloads are raw deflate
(no zlib wrapper), so the two-byte header and four-byte checksum that
zlib.compress adds are stripped.

Frames that do not compress smaller than the frame size are stored raw, both
because it is what real encoders do and because it exercises the reader's
uncompressed path.
"""

import os
import struct
import sys
import zlib

FRAME_SIZE = 2048
HEADER_SIZE = 24
CSO_VERSION = 1
UNCOMPRESSED_FLAG = 0x80000000


def write_cso(src_path, dst_path, frame_size=FRAME_SIZE):
    total = os.path.getsize(src_path)
    frames = (total + frame_size - 1) // frame_size

    header = b'CISO' + struct.pack(
        '<IQIBBBB',
        HEADER_SIZE,   # header_size
        total,         # total_bytes (uncompressed)
        frame_size,    # frame_size
        CSO_VERSION,   # ver
        0,             # align (index shift)
        0, 0)          # reserved

    index_bytes = (frames + 1) * 4
    payload_offset = len(header) + index_bytes

    index = []
    payloads = []
    pos = payload_offset
    raw_frames = 0

    with open(src_path, 'rb') as src:
        for _ in range(frames):
            raw = src.read(frame_size)
            if len(raw) < frame_size:
                raw += b'\0' * (frame_size - len(raw))

            # raw deflate: drop zlib's 2-byte header and 4-byte adler32
            comp = zlib.compress(raw, 1)[2:-4]

            if len(comp) >= frame_size:
                index.append(pos | UNCOMPRESSED_FLAG)
                payloads.append(raw)
                pos += frame_size
                raw_frames += 1
            else:
                index.append(pos)
                payloads.append(comp)
                pos += len(comp)

    index.append(pos)  # terminator: gives the last frame its length

    with open(dst_path, 'wb') as dst:
        dst.write(header)
        for entry in index:
            dst.write(struct.pack('<I', entry))
        for payload in payloads:
            dst.write(payload)

    return total, frames, raw_frames, os.path.getsize(dst_path)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: make_cso.py <input image> <output .cso>\n")
        return 2

    total, frames, raw_frames, out_size = write_cso(argv[1], argv[2])
    print("%s: %d bytes, %d frames (%d stored uncompressed) -> %s, %d bytes (%.1f%%)"
          % (argv[1], total, frames, raw_frames, argv[2], out_size,
             100.0 * out_size / total if total else 0.0))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
