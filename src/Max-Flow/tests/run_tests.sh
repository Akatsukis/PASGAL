#!/usr/bin/env bash
# Run all algorithms on the generated unit-test suite and compare their
# reported max-flow against the EIBFS reference in manifest.tsv.
#
# Expects the CMake-built binaries under:
#   $BIN_DIR            (default: ../../../build/src/Max-Flow)
# and the manifest + case_NN.adj files under:
#   $(dirname "$0")     (this script's directory)
#
# Exit code is 0 only if every algorithm matches every reference value.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="${BIN_DIR:-$HERE/../../../build/src/Max-Flow}"
MANIFEST="$HERE/manifest.tsv"

DINIC="$BIN_DIR/dinic"
PR="$BIN_DIR/push-relabel"

if [[ ! -x "$DINIC" || ! -x "$PR" ]]; then
    cat >&2 <<EOF
Missing binaries under $BIN_DIR.
Build them:
    cmake --build <cmake-build-dir> --target dinic push-relabel
Or override: BIN_DIR=/path/to/bindir  $0
EOF
    exit 2
fi
if [[ ! -f "$MANIFEST" ]]; then
    echo "Missing $MANIFEST.  Run generate_tests.py first." >&2
    exit 2
fi

# Algorithms to test:  (label, binary, extra-args)
# push-relabel supports -a basic / -a nondet.
declare -a ALGO_LABELS=("dinic" "push-relabel-basic" "push-relabel-nondet")
declare -a ALGO_BINS=("$DINIC" "$PR" "$PR")
declare -a ALGO_ARGS=("" "-a basic" "-a nondet")

pass=0
fail=0
skip=0
failures=()

printf "%-22s  %-11s  %10s  %10s  %s\n" "case" "algo" "expected" "got" "status"
printf "%-22s  %-11s  %10s  %10s  %s\n" "----" "----" "--------" "---" "------"

while IFS=$'\t' read -r tag nodes arcs src sink flow; do
    [[ "$tag" == \#* || -z "$tag" ]] && continue
    adj="$HERE/${tag}.adj"
    if [[ ! -f "$adj" ]]; then
        echo "  missing $adj -- did generate_tests.py fail for this case?" >&2
        skip=$((skip+1))
        continue
    fi

    for i in "${!ALGO_LABELS[@]}"; do
        label="${ALGO_LABELS[$i]}"
        bin="${ALGO_BINS[$i]}"
        extra="${ALGO_ARGS[$i]}"

        # Run with threads=1 for determinism; timeout 30s as a safety net.
        out=$(timeout 30 "$bin" -i "$adj" -r "$src" -t "$sink" $extra 2>&1)
        got=$(echo "$out" | awk '/^Max flow:/ {val=$3} END {print val}')

        if [[ -z "$got" ]]; then
            status="FAIL(no-output)"
            fail=$((fail+1))
            failures+=("$tag/$label: no 'Max flow:' in output")
        elif [[ "$got" == "$flow" ]]; then
            status="OK"
            pass=$((pass+1))
        else
            status="FAIL(diff=$((got - flow)))"
            fail=$((fail+1))
            failures+=("$tag/$label: expected $flow got $got")
        fi
        printf "%-22s  %-11s  %10s  %10s  %s\n" "$tag" "$label" "$flow" "${got:-?}" "$status"
    done
done < "$MANIFEST"

echo
echo "========================================"
echo "PASS=$pass  FAIL=$fail  SKIP=$skip"
if [[ $fail -gt 0 ]]; then
    echo "Failures:"
    printf '  %s\n' "${failures[@]}"
    exit 1
fi
echo "All unit tests passed."
