#!/bin/sh

set -eu

binary=$1
backend=$2
work=$(mktemp -d "$PWD/codexbar-cards-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT

output=$(COLUMNS=80 CODEXBAR_BACKEND="$backend" "$binary" cards)
case "$output" in
  *'Codex [oauth] PLAN Pro'*'Claude [cli]'*'@ dev@example.test'*'Session'*'72% left'*'Balance: 12.50 left'*) ;;
  *)
    printf 'unexpected card output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" cards --provider codex --brief)
case "$output" in
  'codexbar AI Usage & Limits'*'Codex'*'session 28% used'*'28 / 100 request'*) ;;
  *)
    printf 'unexpected brief output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" cards --provider codex --no-credits)
case "$output" in
  *Balance*)
    printf 'credits were not suppressed\n' >&2
    exit 1
    ;;
esac

if CODEXBAR_BACKEND="$backend" "$binary" cards --provider unknown >"$work/output" 2>"$work/error"; then
    printf 'unknown provider unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: Unknown provider: unknown' ]

if CODEXBAR_BACKEND="$backend" "$binary" cards --provider codex --account first >"$work/output" 2>"$work/error"; then
    printf 'unsupported account selection unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: Account selection is not available in the native C command yet.' ]

if CODEXBAR_BACKEND="$backend" "$binary" cards --source local >"$work/output" 2>"$work/error"; then
    printf 'invalid source unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: --source must be auto|web|cli|oauth|api.' ]
