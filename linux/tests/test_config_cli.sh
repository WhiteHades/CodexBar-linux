#!/bin/sh

set -eu

binary=$1
work=$(mktemp -d "$PWD/codexbar-config-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
config=$work/config.json

output=$(CODEXBAR_CONFIG="$config" "$binary" config providers)
case "$output" in
  'codex: enabled default (Codex)'*) ;;
  *)
    printf 'unexpected provider list: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_CONFIG="$config" "$binary" config enable --provider or)
[ "$output" = 'Config: enabled OpenRouter' ]
[ "$(stat -c %a "$config")" = 600 ]
[ "$(stat -c %a "$work")" = 700 ]

output=$(printf 'secret-value\n' | CODEXBAR_CONFIG="$config" "$binary" config set-api-key --provider openrouter --stdin)
[ "$output" = 'Config: stored API key for OpenRouter and enabled' ]

if printf 'prefix\0suffix' | CODEXBAR_CONFIG="$config" \
  "$binary" config set-api-key --provider openrouter --stdin >/dev/null 2>&1; then
    printf 'NUL-containing API key unexpectedly succeeded\n' >&2
    exit 1
fi
if printf 'prefix\177suffix' | CODEXBAR_CONFIG="$config" \
  "$binary" config set-api-key --provider openrouter --stdin >/dev/null 2>&1; then
    printf 'control-containing API key unexpectedly succeeded\n' >&2
    exit 1
fi

if CODEXBAR_CONFIG="$config" "$binary" config set-api-key --provider openrouter --api-key --json >/dev/null 2>&1; then
    printf 'missing API key value unexpectedly succeeded\n' >&2
    exit 1
fi
output=$(CODEXBAR_CONFIG="$config" "$binary" config set-api-key --provider openrouter --api-key --json 2>/dev/null || true)
case "$output" in
  '{"error":"Missing value for --api-key."}') ;;
  *)
    printf 'unexpected JSON argument error: %s\n' "$output" >&2
    exit 1
    ;;
esac
if CODEXBAR_CONFIG="$config" "$binary" config providers --bogus >/dev/null 2>&1; then
    printf 'unknown config argument unexpectedly succeeded\n' >&2
    exit 1
fi
if CODEXBAR_CONFIG="$config" "$binary" config providers --format yaml >/dev/null 2>&1; then
    printf 'invalid config format unexpectedly succeeded\n' >&2
    exit 1
fi
if CODEXBAR_CONFIG="$config" "$binary" config set-api-key --provider bedrock --api-key invalid >/dev/null 2>&1; then
    printf 'unsupported Bedrock API key unexpectedly succeeded\n' >&2
    exit 1
fi

output=$(CODEXBAR_CONFIG="$config" "$binary" config validate)
[ "$output" = 'Config: OK' ]

output=$(CODEXBAR_CONFIG="$config" "$binary" config dump)
case "$output" in
  *'"version":1'*'"id":"openrouter"'*'"apiKey":"secret-value"'*) ;;
  *)
    printf 'unexpected config dump\n' >&2
    exit 1
    ;;
esac

cat >"$config" <<'EOF'
{"version":1,"providers":[{"id":"deepseek","enabled":true,"source":"web"}]}
EOF
if CODEXBAR_CONFIG="$config" "$binary" config validate >"$work/validate.out"; then
    printf 'invalid source unexpectedly passed validation\n' >&2
    exit 1
fi
output=$(cat "$work/validate.out")
case "$output" in
  '[ERROR] deepseek (source): Source web is not supported for deepseek.'*) ;;
  *)
    printf 'unexpected validation output: %s\n' "$output" >&2
    exit 1
    ;;
esac
