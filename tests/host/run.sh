#!/bin/sh
# Builds and runs the host tests (the writer and the restore's checks on a
# fake unit; USB MIDI: descriptors and the soft key's packets; the library
# and the switch, on the fake unit) with the PC's
# own compiler: tests/host/run.sh
# (SAN= turns the sanitizers off, for a compiler without their libraries)
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/synth-rescue-host-tests
flags="-std=c++17 -O1 -g -Wall -Wextra -Werror ${SAN--fsanitize=address,undefined} -Isrc -Itests/host -Wl,-z,noexecstack"
${CXX:-c++} $flags src/writer.cpp src/restore.cpp src/rescue_blob.S tests/host/fake_jl.cpp tests/host/test_restore.cpp -o "$out"
${CXX:-c++} $flags src/usb_midi.cpp tests/host/test_usb_midi.cpp -o "$out-usb-midi"
${CXX:-c++} $flags src/library.cpp src/switcher.cpp src/writer.cpp src/restore.cpp src/rescue_blob.S \
    tests/host/fake_jl.cpp tests/host/test_library.cpp -o "$out-library"
"$out"
"$out-usb-midi"
"$out-library"
