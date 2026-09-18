#!/bin/sh
# The chunked CDVD reader hands back the bytes the file holds.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
OBJ="$DIR/.obj"
SANFLAGS="${SANFLAGS:-}"
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $LC/include"
rm -rf "$OBJ" && mkdir -p "$OBJ"

# C and C++ get their own compiler: libretro-common is C and does not
# build as C++.
for c in streams/file_stream vfs/vfs_implementation compat/fopen_utf8 \
         encodings/encoding_utf compat/compat_strl file/file_path \
         file/file_path_io time/rtime string/stdstring string/rstrtod \
         formats/chd/rchd formats/flac/rflac formats/yaml/ryaml \
         formats/7z/r7z_lzma encodings/encoding_rzstd \
         encodings/encoding_huffman encodings/encoding_deflate \
         encodings/encoding_crc32 encodings/encoding_rlz4 \
         streams/trans_stream streams/trans_stream_deflate \
         streams/trans_stream_pipe features/features_cpu; do
	${CC:-cc} -O1 -w $SANFLAGS $INC -c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done

${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS $INC \
	-o "$DIR/cdvd_read" "$DIR/main.cpp" \
	"$ROOT/pcsx2/CDVD/ThreadedFileReader.cpp" "$ROOT/pcsx2/CDVD/InputIsoFile.cpp" \
	"$ROOT/pcsx2/CDVD/FlatFileReader.cpp" "$ROOT/pcsx2/CDVD/CsoFileReader.cpp" \
	"$ROOT/pcsx2/CDVD/GzippedFileReader.cpp" "$ROOT/pcsx2/CDVD/ChdFileReader.cpp" \
	"$ROOT/pcsx2/Pcsx2Config.cpp" "$ROOT/pcsx2/MemoryCardFile.cpp" \
	"$ROOT/pcsx2/StringView.cpp" "$ROOT/pcsx2/FormatString.cpp" \
	"$OBJ"/*.o -lpthread
rm -rf "$OBJ"
"$DIR/cdvd_read" "$DIR"
