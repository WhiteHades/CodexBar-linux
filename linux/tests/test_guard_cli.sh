#!/bin/sh

set -eu

binary=$1
backend=$2
work=$(mktemp -d "$PWD/codexbar-guard-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT

output=$(CODEXBAR_BACKEND="$backend" "$binary" guard --provider codex)
[ "$output" = "codex session: 72% remaining — OK (minimum 10%)" ]

set +e
output=$(CODEXBAR_BACKEND="$backend" "$binary" guard --provider codex --min-remaining 73 --json)
status=$?
set -e
[ "$status" -eq 1 ]
[ "$output" = '{"provider":"codex","window":"session","remainingPercent":72.0,"minimumRemainingPercent":73.0,"decision":"blocked","exitCode":1,"unavailableReason":null}' ]

output=$(CODEXBAR_BACKEND="$backend" "$binary" guard --provider codex --window weekly --json --pretty)
case "$output" in
  *'"window":"weekly"'*'"remainingPercent":28.'*'"decision":"ok"'*) ;;
  *)
    printf 'unexpected weekly guard output: %s\n' "$output" >&2
    exit 1
    ;;
esac

set +e
output=$(CODEXBAR_BACKEND="$backend" "$binary" guard --provider openai --json)
status=$?
set -e
[ "$status" -eq 69 ]
case "$output" in
  *'"decision":"unknown"'*'"exitCode":69'*'"unavailableReason":"fetch-failed"'*) ;;
  *)
    printf 'unexpected unavailable guard output: %s\n' "$output" >&2
    exit 1
    ;;
esac

CODEXBAR_BACKEND="$backend" "$binary" guard --provider openai --fail-open >/dev/null

slow_backend=$work/slow-backend.sh
cp "$backend" "$slow_backend"
sed -i '3i sleep 2' "$slow_backend"
chmod +x "$slow_backend"
set +e
output=$(CODEXBAR_BACKEND="$slow_backend" "$binary" guard --provider codex --timeout 0.01 --json)
status=$?
set -e
[ "$status" -eq 69 ]
case "$output" in
  *'"unavailableReason":"timeout"'*) ;;
  *)
    printf 'unexpected timeout guard output: %s\n' "$output" >&2
    exit 1
    ;;
esac

for arguments in \
  '--provider codex --window monthly' \
  '--provider codex --min-remaining nan' \
  '--provider codex --min-remaining 101' \
  '--provider codex --timeout -1' \
  '--provider both' \
  '--provider unknown' \
  '--unknown'; do
    set +e
    # shellcheck disable=SC2086
    "$binary" guard $arguments >"$work/args-out" 2>"$work/args-error"
    status=$?
    set -e
    [ "$status" -eq 64 ]
done
