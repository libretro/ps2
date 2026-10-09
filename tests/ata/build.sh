#!/bin/sh
# The DEV9 HDD's IO thread handshake, on the real ATA translation units:
# queued writes, asynchronous and synchronous reads, and a close with
# writes still queued; and host read and write failures ending the
# guest's commands with ATA errors. Plain and TSan; MSYS2 MINGW64 builds
# and runs the plain ones.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
ATA="$ROOT/pcsx2/DEV9/ATA"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1; EXE=.exe; LIBS="" ;;
  *)                    WINDOWS=0; EXE="";   LIBS="-lpthread" ;;
esac

COMMON="$ATA/ATA_State.c $ATA/ATA_Transfer.c $ATA/ATA_Info.c \
  $ATA/Commands/ATA_Command.c $ATA/Commands/ATA_CmdDMA.c \
  $ATA/Commands/ATA_CmdExecuteDeviceDiag.c $ATA/Commands/ATA_CmdNoData.c \
  $ATA/Commands/ATA_CmdPIOData.c $ATA/Commands/ATA_CmdSMART.c \
  $ATA/Commands/ATA_SCE.c \
  $LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c \
  $LC/rthreads/retro_procbarrier.c \
  $LC/streams/file_stream.c $LC/vfs/vfs_implementation.c \
  $LC/file/file_path.c $LC/file/file_path_io.c \
  $LC/string/stdstring.c $LC/string/rstrtod.c $LC/compat/compat_strl.c \
  $LC/encodings/encoding_utf.c $LC/compat/fopen_utf8.c $LC/time/rtime.c"
INC="-I $LC/include -I $ROOT/pcsx2 -I $ATA"
FLAGS="-std=gnu99 -Wall -Wno-unused-function -D_GNU_SOURCE -DHAVE_THREADS"

build() { # output, main source, extra flags
	"$CC" $FLAGS $3 $INC -o "$DIR/$1$EXE" "$2" $COMMON $LIBS
}

# The image is written to the current directory; run from the suite's.
cd "$DIR"

build ata_test "$DIR/main.c" "-O2 $SANFLAGS"
"$DIR/ata_test$EXE"
build ata_ioerror "$DIR/ioerror.c" "-O2 $SANFLAGS"
"$DIR/ata_ioerror$EXE"

if [ "$WINDOWS" != "1" ]; then
	build ata_test_tsan "$DIR/main.c" "-O1 -g -fsanitize=thread -Wno-tsan"
	"$DIR/ata_test_tsan"
	build ata_ioerror_tsan "$DIR/ioerror.c" "-O1 -g -fsanitize=thread -Wno-tsan"
	"$DIR/ata_ioerror_tsan"
fi
