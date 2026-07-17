#!/bin/sh

set -eu

binary=$1
backend=$2
work=$(mktemp -d "$PWD/codexbar-usage-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
config=$work/config.json

output=$(CODEXBAR_BACKEND="$backend" "$binary")
case "$output" in
  *'codex · dev@example.com · Pro [oauth]'*'session: 28% used'*'claude [cli]'*) ;;
  *)
    printf 'unexpected default usage output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider codex --format json)
case "$output" in
  '[{"provider":"codex"'*'"primary":{"usedPercent":28'*'"credits":{"remaining":12.5}'*) ;;
  *)
    printf 'unexpected codex JSON output: %s\n' "$output" >&2
    exit 1
    ;;
esac

root_output=$(CODEXBAR_BACKEND="$backend" "$binary" --provider codex --json)
[ "$root_output" = "$output" ]

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider both --json)
case "$output" in
  '[{"provider":"codex"'*'},{"provider":"claude"'*) ;;
  *)
    printf 'unexpected combined JSON output: %s\n' "$output" >&2
    exit 1
    ;;
esac

if CODEXBAR_BACKEND="$backend" "$binary" usage --provider deepseek --source web >/dev/null 2>&1; then
    printf 'unsupported source unexpectedly succeeded\n' >&2
    exit 1
fi

if CODEXBAR_BACKEND="$backend" "$binary" usage --provider unknown >/dev/null 2>&1; then
    printf 'unknown provider unexpectedly succeeded\n' >&2
    exit 1
fi

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider unknown --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"cli","source":"cli","error":{"message":"Unknown provider: unknown","code":1,"kind":"args"}}]') ;;
  *)
    printf 'unexpected JSON error output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider BOTH --json)
case "$output" in
  '[{"provider":"codex"'*'},{"provider":"claude"'*) ;;
  *)
    printf 'uppercase provider keyword was not normalized\n' >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider codex --json --format text)
case "$output" in
  'codex · dev@example.com · Pro [oauth]'*) ;;
  *)
    printf 'explicit text format did not override JSON shortcut\n' >&2
    exit 1
    ;;
esac

[ "$("$binary" --version)" = 'CodexBar 0.1.0' ]

output=$(env -u CODEXBAR_BACKEND -u KIMI_CODE_API_KEY CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider kimi --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"kimi","source":"api","error":{"message":"Kimi Code API key is missing.'*) ;;
  *)
    printf 'unexpected native Kimi missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider kimi --source web --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"kimi","source":"web","error":{"message":"Kimi source '\''web'\'' has no native Linux implementation yet"'*) ;;
  *)
    printf 'unexpected native Kimi web-source output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND -u KIMI_K2_API_KEY -u KIMI_API_KEY -u KIMI_KEY \
  CODEXBAR_CONFIG="$config" "$binary" usage --provider kimi-k2 --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"kimik2","source":"api","error":{"message":"Missing Kimi K2 API key."'*) ;;
  *)
    printf 'unexpected native Kimi K2 missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac
