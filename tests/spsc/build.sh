#!/bin/sh
# retro_spsc stress: plain + TSan builds.
# Usage: sh tests/spsc/build.sh  (from repo root)
set -e
INC="-I libretro/libretro-common/include"
gcc -O2 -std=c99 -Wall $INC -o tests/spsc/spsc_test \
  tests/spsc/main.c libretro/libretro-common/queues/retro_spsc.c -lpthread
gcc -O1 -g -std=c99 -Wall -fsanitize=thread $INC -o tests/spsc/spsc_test_tsan \
  tests/spsc/main.c libretro/libretro-common/queues/retro_spsc.c -lpthread
echo "built: tests/spsc/spsc_test tests/spsc/spsc_test_tsan"
