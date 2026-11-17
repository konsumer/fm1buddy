#!/bin/sh
# Installs a package on the simulator's pretend FM-1: once as it should go, then with faults (lost and repeated
# requests, garbled answers, stray SysEx, unplugged while writing). Each must end with the FM-1's flash the same as
# the package, with the package in memory and (--lowmem) read from the SD card as it goes. Uses the first .fwsc in sim/sd/fm1 (or FM1_SD/fm1); with none, the Hub's stock firmware is got first.
set -e
cd "$(dirname "$0")/.."
sd=${FM1_SD:-sim/sd}
mkdir -p "$sd/fm1"
pkg=$(ls "$sd/fm1" | grep -i '\.fwsc$' | head -1 || true)
if [ -z "$pkg" ]; then
  url=$(curl -fsS https://mutant.cloud/catalog.json | python3 -I -c '
import json, sys
print(json.load(sys.stdin)["stock"]["pkg"])')
  pkg=$(basename "$url")
  curl -fsS -o "$sd/fm1/$pkg" "$url"
fi
pio run -s -e sim
for how in "" --faults --lowmem "--lowmem --faults"; do
  echo "install $pkg ${how:-(no faults)}"
  FM1_SD="$sd" .pio/build/sim/program --headless $how --install "$pkg" > build/install-test.log 2>&1 || {
    grep -E 'sim:|job:|fake fm-1: (flash|unplugged|A BAD)' build/install-test.log
    exit 1
  }
  grep -E 'fake fm-1: (flash|unplugged|A BAD)|sim: install, try' build/install-test.log
done
echo ok
