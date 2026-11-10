#!/bin/sh
# Builds and runs the tests of the app's plain logic (test/*_test.cpp), on the computer
set -e
cd "$(dirname "$0")/.."
mkdir -p build
c++ -std=gnu++17 -Wall -I src/app -o build/midimap_test test/midimap_test.cpp src/app/midimap.cpp
build/midimap_test
c++ -std=gnu++17 -Wall -I src/app -o build/sloopfmt_test test/sloopfmt_test.cpp src/app/sloopfmt.cpp
build/sloopfmt_test
c++ -std=gnu++17 -Wall -I src/app -o build/mirror_test test/mirror_test.cpp src/app/mirror.cpp
build/mirror_test
