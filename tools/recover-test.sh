#!/bin/bash
# Backs up and recovers the simulator's pretend FM-1, as it is and with a board's memory and no PSRAM (--lowmem: the
# flash and the package stay on the SD card, FlashBuf). Each pair must leave the same flash, and the same backup.
# Recovers from a package (the first .fwsc in sim/sd/fm1), and from backups made here that differ from its flash (an
# FM-1's, and an SMK-37's with its own calibration, which must stay as it is).
set -e
cd "$(dirname "$0")/.."
pio run -s -e sim
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pkg=$(ls sim/sd/fm1 | grep -i '\.fwsc$' | head -1)
# a backup that differs: data sectors changed, half a sector, its head; smk: another keyboard's calibration
mkbak() {
  python3 -I - "$1" "$2" "$3" <<'PY'
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
for a in range(0x10000, 0x30000):
    b[a] ^= 0x5A
b[0x80000:0x80800] = bytes(0x800)
b[0x100] ^= 0xFF
if sys.argv[3] == '1':
    b[0xC1000:0xC2000] = bytes([0x33]) * 0x1000
    b[0xC1224:0xC122B] = b'trimdat'
open(sys.argv[2], 'wb').write(b)
PY
}
# one run on a fresh copy of the card: prints its flash's sha256 (and the backup's, for --backup)
run() {
  rm -rf "$work/sd" && cp -R sim/sd "$work/sd"
  bak=$(ls sim/sd/backups | grep '\.bin$' | head -1)
  mkbak "sim/sd/backups/$bak" "$work/sd/backups/fm1-90.bin" 0
  mkbak "sim/sd/backups/$bak" "$work/sd/backups/smk-91.bin" 1
  FM1_SD="$work/sd" .pio/build/sim/program --headless "$@" > "$work/log" 2>&1 || {
    grep -E 'sim:|job:|recover:' "$work/log"
    exit 1
  }
  saved=$(grep 'sim: backup: ok' "$work/log" | awk '{print $NF}')
  echo "$(grep 'sim: flash' "$work/log" | awk '{print $NF}') ${saved:+$(shasum -a 256 "$work/sd/backups/$saved" | cut -c1-64)}"
  if ls "$work/sd/fm1buddy" | grep -q '\.tmp$'; then
    echo "a .tmp was left on the card" >&2
    exit 1
  fi
}
for args in "--backup" "--recover /fm1/$pkg" "--recover /backups/fm1-90.bin" "--smk --recover /backups/smk-91.bin"; do
  a=$(run $args)
  b=$(run $args --lowmem)
  echo "$args: $a"
  if [ "$a" != "$b" ]; then
    echo "  --lowmem: $b: NOT THE SAME"
    exit 1
  fi
done
echo ok
