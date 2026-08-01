#!/bin/sh
set -eu

binary=$1
work=$(mktemp -d "$PWD/codexbar-cookie-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

if "$binary" cookie refresh --provider codex >"$work/output" 2>"$work/error"; then
    printf 'Linux cookie refresh unexpectedly succeeded\n' >&2
    exit 1
fi
[ ! -s "$work/output" ]
[ "$(sed -n '1p' "$work/error")" = "Cookie refresh is only supported on macOS." ]

if "$binary" cookie refresh --provider unknown >/dev/null 2>"$work/error"; then
    printf 'unknown cookie provider unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(sed -n '1p' "$work/error")" = "Unknown provider." ]

if "$binary" cookie refresh --all --provider codex >/dev/null 2>"$work/error"; then
    printf 'ambiguous cookie selection unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(sed -n '1p' "$work/error")" = "Specify exactly one of --provider <name> or --all." ]
