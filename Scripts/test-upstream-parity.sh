#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
check=$repo/Scripts/check-upstream-parity.sh
ledger=$repo/audit/upstream-parity.tsv
tmp=$(mktemp -d "${TMPDIR:-/tmp}/codexbar-upstream-parity-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

expect_failure() {
    label=$1
    candidate=$2
    if "$check" "$candidate" >/dev/null 2>&1; then
        printf 'upstream parity check accepted %s ledger\n' "$label" >&2
        exit 1
    fi
}

"$check" "$ledger" >/dev/null
if "$check" --require-complete "$ledger" >/dev/null 2>&1; then
    printf '%s\n' 'upstream parity check accepted pending rows as complete' >&2
    exit 1
fi

cp "$ledger" "$tmp/duplicate.tsv"
awk -F '\t' '!/^#/ && !/^$/ {print; exit}' "$ledger" >>"$tmp/duplicate.tsv"
expect_failure duplicate "$tmp/duplicate.tsv"

awk -F '\t' 'BEGIN {removed = 0} /^#/ || /^$/ {print; next} !removed {removed = 1; next} {print}' \
    "$ledger" >"$tmp/missing.tsv"
expect_failure missing "$tmp/missing.tsv"

base=$(awk -F '\t' '$1 == "# base" {print $2}' "$ledger")
outside=$(git -C "$repo" rev-parse "$base^1")
awk -F '\t' -v outside="$outside" 'BEGIN {OFS = "\t"; replaced = 0} /^#/ || /^$/ {print; next} \
    !replaced {$1 = outside; replaced = 1} {print}' "$ledger" >"$tmp/outside.tsv"
expect_failure out-of-range "$tmp/outside.tsv"

awk -F '\t' 'BEGIN {OFS = "\t"; changed = 0} /^#/ || /^$/ {print; next} \
    !changed {$2 = "complete"; changed = 1} {print}' "$ledger" >"$tmp/disposition.tsv"
expect_failure invalid-disposition "$tmp/disposition.tsv"

printf '%s\n' 'upstream parity invariant tests passed'
