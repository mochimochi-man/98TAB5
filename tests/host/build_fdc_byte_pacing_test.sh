#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tests/host"
OUT="${OUT:-/tmp/pc98-fdc-byte-pacing}"
OBJ="$OUT/obj"
EXTRA_SOURCES="$HERE/shim_machine/host_platform.c $HERE/fdc_byte_pacing_test.c $(ls "$ROOT"/components/pc98core/cpu/*.c)"
. "$HERE/machine_build.inc"
# The regression includes production fdc765.c to inspect DRQ/byte deadlines.
FORCE=1 machine_compile
rm -f "$OBJ/components_pc98core_dev_fdc765.c.o"
gcc -o "$OUT/test" "$OBJ"/*.o -lm
mkdir -p "$OUT/sd"
PC98_SD="$OUT/sd" "$OUT/test"