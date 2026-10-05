#!/bin/sh
# The demo knob's built-in synth profiles (src/demo_synths.json): the firmware's own, as
# midi_synths.c writes them, in its order. Rerun after a built-in synth changes, then retake the
# screenshots.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FW="$HERE/../../NanoDepsidf"
TMP=$(mktemp -d)
"$FW/tools/midi_synth_test/run.sh" "$TMP" > /dev/null
python3 - "$TMP" "$HERE/../src/demo_synths.json" <<'PY'
import json, os, sys
tmp, out = sys.argv[1], sys.argv[2]
n = len([f for f in os.listdir(tmp) if f.endswith(".json")])
synths = [json.load(open(f"{tmp}/{i}.json")) for i in range(n)]
json.dump(synths, open(out, "w"), separators=(",", ":"))
print(f"{out}: {', '.join(s['id'] for s in synths)}")
PY
rm -rf "$TMP"
