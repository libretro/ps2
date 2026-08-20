#!/bin/sh
# Differential test: the EE COP2 flag-hack analysis, x86 vs arm64.
# Builds the harness (which extracts both implementations from the recompiler
# sources) and runs it. Exit status is non-zero if the arm64 side marks FEWER
# flags than x86 anywhere -- the direction that can lose sticky VU0 status.
set -e
dir=$(dirname "$0")
${CXX:-g++} -O1 -std=c++17 -o "$dir/cop2_flag_diff" "$dir/cop2_flag_diff.cpp"
exec "$dir/cop2_flag_diff"
