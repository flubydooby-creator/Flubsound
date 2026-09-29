#!/usr/bin/env bash
# Flubsound Pro - runs every libFuzzer target for a fixed time (docs/11 E53).
#
#   tests/fuzz/run-fuzzers.sh <build dir> [seconds per target, default 30] [work dir]
#
# The targets run in parallel, each on a working corpus seeded from
# tests/fuzz/corpus/<name>, the factory presets and the device-profile
# database (the JSON targets). New inputs stay in <work dir>/corpus-<name>
# (default: a temporary folder), so a later run with the same work dir
# continues from them. A crash, sanitizer report or failed FUZZ_CHECK is
# saved as <work dir>/<name>-crash-<sha1> and fails the run (exit 1);
# reproduce it with `<build dir>/tests/fuzz/<name> <that file>`.
set -euo pipefail

build=${1:?usage: run-fuzzers.sh <build dir> [seconds per target] [work dir]}
seconds=${2:-30}
work=${3:-$(mktemp -d)}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
mkdir -p "$work"

targets=(fuzz_json fuzz_preset fuzz_state fuzz_eqtext)
declare -A seeds=(
    [fuzz_json]="json" [fuzz_preset]="preset" [fuzz_state]="state" [fuzz_eqtext]="eqtext")
declare -A dicts=(
    [fuzz_json]="json.dict" [fuzz_preset]="json.dict" [fuzz_state]="json.dict" [fuzz_eqtext]="eqtext.dict")

pids=()
for t in "${targets[@]}"; do
    bin="$build/tests/fuzz/$t"
    [ -x "$bin" ] || { echo "error: $bin not found (configure with -DFLUB_BUILD_FUZZERS=ON)" >&2; exit 2; }
    corpus="$work/corpus-$t"
    mkdir -p "$corpus"
    seedFiles=("$here/corpus/${seeds[$t]}"/*)
    if [ "$t" != fuzz_eqtext ]; then
        seedFiles+=("$root"/presets/factory/*.json "$root/presets/devices/device-profiles.json")
    fi
    for f in "${seedFiles[@]}"; do
        [ -e "$corpus/$(basename "$f")" ] || cp "$f" "$corpus/"
    done
    # -max_len 16384: every seed fits; longer inputs only slow the search.
    "$bin" "$corpus" -dict="$here/${dicts[$t]}" -max_total_time="$seconds" -max_len=16384 \
        -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 -artifact_prefix="$work/$t-" \
        > "$work/$t.log" 2>&1 &
    pids+=($!)
done

status=0
for i in "${!targets[@]}"; do
    t=${targets[$i]}
    if wait "${pids[$i]}"; then
        runs=$(sed -nE 's/^stat::number_of_executed_units:[[:space:]]*([0-9]+).*/\1/p' "$work/$t.log")
        echo "$t: ok, ${runs:-?} runs in ${seconds} s, corpus $(ls "$work/corpus-$t" | wc -l) inputs"
    else
        status=1
        echo "$t: FAILED (log: $work/$t.log)"
        grep -E "ERROR|FUZZ_CHECK|runtime error|Test unit written" "$work/$t.log" | head -n 20 || true
    fi
done
exit $status
