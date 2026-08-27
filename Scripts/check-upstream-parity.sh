#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ledger=$repo/audit/upstream-current.tsv
require_complete=false

if test "${1:-}" = "--require-complete"; then
    require_complete=true
    shift
fi
if test "$#" -gt 1; then
    printf 'usage: %s [--require-complete] [ledger]\n' "$0" >&2
    exit 2
fi
if test "$#" -eq 1; then
    ledger=$1
fi
if test ! -f "$ledger"; then
    printf 'upstream parity ledger not found: %s\n' "$ledger" >&2
    exit 1
fi

metadata_count=$(awk -F '\t' '$1 == "# base" || $1 == "# target" {count++} END {print count + 0}' "$ledger")
base=$(awk -F '\t' '$1 == "# base" {print $2}' "$ledger")
target=$(awk -F '\t' '$1 == "# target" {print $2}' "$ledger")
if test "$metadata_count" -ne 2 || test -z "$base" || test -z "$target"; then
    printf '%s\n' 'ledger must contain exactly one # base and one # target row' >&2
    exit 1
fi
for hash in "$base" "$target"; do
    if test "${#hash}" -ne 40 || printf '%s\n' "$hash" | grep -q '[^0-9a-f]'; then
        printf 'invalid range hash: %s\n' "$hash" >&2
        exit 1
    fi
    if ! git -C "$repo" cat-file -e "$hash^{commit}" 2>/dev/null; then
        printf 'range commit is unavailable: %s\n' "$hash" >&2
        exit 1
    fi
done
if ! git -C "$repo" merge-base --is-ancestor "$base" "$target"; then
    printf 'base is not an ancestor of target: %s %s\n' "$base" "$target" >&2
    exit 1
fi

if ! awk -F '\t' '
    /^#/ || /^$/ { next }
    {
        valid = 1
        if (NF != 4) {
            printf "invalid ledger row %d: expected four tab-separated fields\n", NR > "/dev/stderr"
            valid = 0
        }
        if (length($1) != 40 || $1 ~ /[^0-9a-f]/) {
            printf "invalid commit hash on ledger row %d: %s\n", NR, $1 > "/dev/stderr"
            valid = 0
        }
        if ($2 != "ported" && $2 != "already-covered" && $2 != "non-linux" &&
            $2 != "superseded" && $2 != "pending") {
            printf "invalid disposition on ledger row %d: %s\n", NR, $2 > "/dev/stderr"
            valid = 0
        }
        if ($3 == "" || $4 == "") {
            printf "empty finding or evidence on ledger row %d\n", NR > "/dev/stderr"
            valid = 0
        }
        if ($2 != "pending" && $4 == "-") {
            printf "completed disposition lacks evidence on ledger row %d\n", NR > "/dev/stderr"
            valid = 0
        }
        if (!valid) failed = 1
    }
    END { exit failed }
' "$ledger"; then
    exit 1
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/codexbar-upstream-parity.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
awk -F '\t' '!/^#/ && !/^$/ {print $1}' "$ledger" >"$tmp/actual"
duplicates=$(sort "$tmp/actual" | uniq -d)
if test -n "$duplicates"; then
    printf '%s\n%s\n' 'duplicate ledger commits:' "$duplicates" >&2
    exit 1
fi

{
    printf '%s\n' "$base"
    git -C "$repo" rev-list "$base..$target"
} | sort -u >"$tmp/expected"
sort -u "$tmp/actual" >"$tmp/actual-sorted"
missing=$(comm -23 "$tmp/expected" "$tmp/actual-sorted")
extra=$(comm -13 "$tmp/expected" "$tmp/actual-sorted")
if test -n "$missing"; then
    printf '%s\n%s\n' 'commits missing from ledger:' "$missing" >&2
fi
if test -n "$extra"; then
    printf '%s\n%s\n' 'ledger commits outside target range:' "$extra" >&2
fi
if test -n "$missing" || test -n "$extra"; then
    exit 1
fi

total=$(wc -l <"$tmp/actual")
printf 'upstream parity range: %s..%s (inclusive)\n' "$base" "$target"
printf 'commits: %s\n' "$total"
for disposition in ported already-covered non-linux superseded pending; do
    count=$(awk -F '\t' -v disposition="$disposition" '$2 == disposition {count++} END {print count + 0}' "$ledger")
    printf '%s: %s\n' "$disposition" "$count"
    if test "$disposition" = pending; then
        pending=$count
    fi
done
if test "$require_complete" = true && test "$pending" -ne 0; then
    printf '%s\n' 'upstream parity audit still has pending commits' >&2
    exit 1
fi
