#!/bin/bash
# Golana (port SMK-37) nel browser: il wasm con l'ABI della pagina dell'emulatore
# (synth_init/render/midi, fw_event/release/screen/status/snapshot/restore), la stessa
# che usano le altre istanze in ~/smk-37_cf/emu/page/audio/.
#
#   sh web/emu/build_smk_emu.sh            -> build/emu-smk/golana-smk-engine.wasm
#
# Il ponte e' web/emu/smk_engine.c sopra golana_smk_web.c (generato da felucca_web.c da
# gen_smk_web.py: pannello SMK al posto di quello FM-1). Prima serve una build
# (`sh build.sh` o BOARD=smk37 sh build.sh): i file generati stanno in build/gen.
set -e
cd "$(dirname "$0")/../.."
if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    . "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1 || true
fi
command -v emcc >/dev/null 2>&1 || { echo "emu-smk: emcc non trovato (. ~/emsdk/emsdk_env.sh)"; exit 1; }
[ -f build/gen/felucca_tables.h ] || { echo "emu-smk: esegui prima ./build.sh (build/gen)"; exit 1; }

python3 web/emu/gen_smk_web.py >/dev/null
VERSION=$(sed -n 's/^#define FELUCCA_VERSION "\([^"]*\)".*/\1/p' firmware/src/felucca.c)
OUT=build/emu-smk
mkdir -p "$OUT"

EXPORTS="synth_init synth_midi synth_render fw_event fw_release fw_screen fw_status fw_request fw_reply fw_command fw_changed fw_snapshot fw_snapshot_size fw_restore"
WEXPORTS=""
for e in $EXPORTS; do WEXPORTS="$WEXPORTS -Wl,--export=$e"; done
# shellcheck disable=SC2086
emcc ${EMU_OPT:--O2} -ffp-contract=off -std=gnu99 -w ${EMU_CFLAGS:-} \
    "-DFELUCCA_VERSION=\"$VERSION\"" -Ibuild/gen -Ifirmware/src -Ifirmware/hal $WEXPORTS \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 \
    -o "$OUT/golana-smk-engine.wasm" web/emu/smk_engine.c

python3 - "$OUT" <<'PY'
import hashlib, json, sys
from pathlib import Path
out = Path(sys.argv[1])
wasm = (out / "golana-smk-engine.wasm").read_bytes()
(out / "golana-smk-build.json").write_text(
    json.dumps({"schema": 1, "sha256": hashlib.sha256(wasm).hexdigest(), "bytes": len(wasm)}) + "\n",
    encoding="utf-8")
print("emu-smk: wasm", len(wasm), "B, sha256", hashlib.sha256(wasm).hexdigest())
PY
echo "emu-smk: $OUT/golana-smk-engine.wasm (Golana $VERSION)"
