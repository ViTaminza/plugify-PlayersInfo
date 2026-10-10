#!/bin/bash
# Assembles build/package/PlayersInfo: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/PlayersInfo"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/PlayersInfo/libPlayersInfo.so" "$OUT/bin/"
cp "$ROOT/build/plugins/PlayersInfo/PlayersInfo.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
