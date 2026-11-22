#!/usr/bin/env bash
# Recompiles the game's code from YOUR copy of the game.
#
#   ./scripts/generate.sh /path/to/default.xbe
#
# default.xbe comes from your own Conker: Live and Reloaded disc image, e.g.
#   extract-xiso -x "Conker.iso" -d game     (or: xdvdfs unpack Conker.iso game)
#
# Output: conker/src/recomp/gen/ (C source, ~140 MB). It is derived from the
# game and must never be committed or shared.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XBE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
[ -f "$XBE" ] || { echo "usage: $0 /path/to/default.xbe"; exit 1; }
PY="${PYTHON:-python3}"
"$PY" -c "import capstone" 2>/dev/null || { echo "pip install capstone"; exit 1; }

GEN="$ROOT/conker/src/recomp/gen"
cd "$ROOT/xboxrecomp"
mkdir -p game_files
cp "$XBE" game_files/default.xbe

echo "== analysis (parser, disassembly, function identification, ABI)"
"$PY" -m tools.disasm game_files/default.xbe --text-only \
      --extra-sections D3D,D3DX,DSOUND,EFFECTS_,EFFECT_C,EFFECT_U,PAGELK,WMADEC,XACTENG,XGRPH,XMV,XNET,XONLINE,XPP \
      --seed-functions "$ROOT/conker/config/seed_functions.json"
"$PY" -m tools.func_id game_files/default.xbe
"$PY" -m tools.abi_analysis game_files/default.xbe

echo "== recompilation"
# every function the port defines by hand (overrides and sub_X_gen wrappers)
MANUAL="$(mktemp)"; trap 'rm -f "$MANUAL"' EXIT
cat "$ROOT"/conker/src/*.c > "$MANUAL"
# one "extern void sub_X_gen(void);" line per wrapped body, the form the scanner reads
grep -ho 'sub_[0-9A-F]\{8\}_gen' "$ROOT"/conker/src/*.c | sort -u \
    | sed 's/.*/extern void &(void);/' >> "$MANUAL"
find "$GEN" -name 'recomp_*' -delete 2>/dev/null || true
mkdir -p "$GEN"
"$PY" -m tools.recomp game_files/default.xbe --all --game-name conker \
      --split 250 --gen-dir "$GEN" -o "$GEN" \
      --exclude-manual "$MANUAL"
cp templates/runtime/recomp_types.h "$GEN/recomp_types.h"

echo "== port edits"
"$PY" "$ROOT/conker/tools/patch_gen.py" "$GEN"
echo "generated code ready in conker/src/recomp/gen"
