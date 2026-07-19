#!/bin/sh

set -eu

binary=$1
base=$PWD/.tmp/openai-api-cli
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"; rmdir "$base" 2>/dev/null || true' EXIT
export HOME="$work/home"
export XDG_CONFIG_HOME="$work/config"
unset OPENAI_ADMIN_KEY OPENAI_API_KEY OPENAI_PROJECT_ID
mkdir -p "$HOME" "$XDG_CONFIG_HOME"

if output=$("$binary" usage --provider openai --source api --format json); then
    printf 'openai api usage succeeded without a key\n' >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q '"provider":"openai"'
printf '%s\n' "$output" | grep -q '"source":"api"'
printf '%s\n' "$output" | grep -q 'OPENAI_ADMIN_KEY'
