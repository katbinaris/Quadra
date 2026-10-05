#!/bin/sh
# Builds and runs the synth profile host test (src/midi_synths.c) with the Mac's compiler. Needs
# managed_components/ (any firmware build fetches it). Stored synths go to a temporary folder.
# Optional: a folder to write each built-in's JSON into (companion/scripts/gen_demo_synths.sh).
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/midi_synth_test"
DIR=/tmp/midi_synth_test_fs
rm -rf "$DIR"
cc -std=gnu11 -Wall -Wno-unused-function -O1 -g -fsanitize=undefined -Wno-deprecated-declarations \
    -I tools/midi_synth_test/stub -I src -I managed_components/espressif__cjson/cJSON \
    -DMIDI_SYNTH_DIR="\"$DIR\"" \
    tools/midi_synth_test/test.c src/midi_synths.c managed_components/espressif__cjson/cJSON/cJSON.c -o "$OUT"
"$OUT" "$DIR" "$@"
