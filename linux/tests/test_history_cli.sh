#!/bin/sh
set -eu

binary=$1
mkdir -p .tmp
work=$(mktemp -d .tmp/codexbar-history-cli-XXXXXX)
trap 'rm -rf "$work"' EXIT INT TERM
export CODEXBAR_HISTORY_DIR="$work/history"
mkdir -p "$CODEXBAR_HISTORY_DIR"

printf '%s\n' '{"version":1,"preferredAccountKey":null,"unscoped":[{"name":"session","windowMinutes":300,"entries":[{"capturedAt":"2026-08-01T00:00:00.000000Z","usedPercent":42,"resetsAt":null}]}],"accounts":{},"sessionEquivalentWindowPairIdentities":{}}' >"$CODEXBAR_HISTORY_DIR/codex.json"

json=$($binary history --provider codex --format json)
case "$json" in
  *'"version":1'*'"codex"'*'"usedPercent":42'*) ;;
  *) printf 'unexpected history JSON: %s\n' "$json" >&2; exit 1 ;;
esac

text=$($binary history --provider codex)
[ "$text" = 'codex: 1 series, 1 samples' ]

empty=$($binary history --provider claude)
[ "$empty" = 'No plan-utilization history recorded.' ]

if $binary history --provider missing >"$work/out" 2>"$work/error"; then
  echo 'unknown provider unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'Unknown provider: missing' "$work/error"
