#!/bin/sh
# Runs the recompiled game core headless for a few thousand frames:
# accepts the splash screen, selects alternative start 1 and opens some
# screens.  Fails if the runtime aborts.
set -e
EXE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
SRC=$(cd "$(dirname "$0")/.." && pwd)
DIR=$(mktemp -d)
cp "$SRC/ffesdl2.cfg" "$DIR/"
cd "$DIR"
FFE_KEYS="20:0x15,300:0x02,800:0x3c,1100:0x3d,1400:0x3b" \
FFE_FRAMES=3000 FFE_SHOTS=1000,2000 "$EXE"
test -s shot_last.ppm
echo "headless test passed (screenshots in $DIR)"
