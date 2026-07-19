#!/bin/sh

set -eu

binary=$1
base=$PWD/.tmp/zai-cli
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"; rmdir "$base" 2>/dev/null || true' EXIT
export HOME="$work/home"
export XDG_CONFIG_HOME="$work/config"
unset Z_AI_API_KEY Z_AI_API_HOST Z_AI_QUOTA_URL
mkdir -p "$HOME" "$XDG_CONFIG_HOME"

if output=$("$binary" usage --provider zai --source api --format json); then
    printf 'z.ai usage succeeded without a token\n' >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q '"provider":"zai"'
printf '%s\n' "$output" | grep -q '"source":"api"'
printf '%s\n' "$output" | grep -q 'Z_AI_API_KEY'
