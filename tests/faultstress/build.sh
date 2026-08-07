#!/bin/sh
# Concurrent page-fault dispatch stress: plain + TSan builds.
# Usage: sh tests/faultstress/build.sh   (from repo root)
set -e
gcc -O2 -g -std=gnu99 -Wall -o tests/faultstress/fault_test \
  tests/faultstress/main.c -lpthread
gcc -O1 -g -std=gnu99 -Wall -fsanitize=thread \
  -o tests/faultstress/fault_test_tsan \
  tests/faultstress/main.c -lpthread
echo "built: tests/faultstress/fault_test tests/faultstress/fault_test_tsan"
