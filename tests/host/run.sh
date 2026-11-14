#!/bin/sh
# Builds and runs the host tests (the writer and the restore's checks on a
# fake unit) with the PC's own compiler: tests/host/run.sh
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/synth-rescue-host-tests
${CXX:-c++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc -Itests/host -Wl,-z,noexecstack \
    src/writer.cpp src/restore.cpp src/rescue_blob.S tests/host/fake_jl.cpp tests/host/test_restore.cpp -o "$out"
"$out"
