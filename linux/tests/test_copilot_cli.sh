#!/bin/sh

set -eu

binary=$1
base=$PWD/.tmp/copilot-cli
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"; rmdir "$base" 2>/dev/null || true' EXIT
export HOME="$work/home"
export XDG_CONFIG_HOME="$work/config"
unset COPILOT_API_TOKEN
mkdir -p "$HOME" "$XDG_CONFIG_HOME"

if output=$("$binary" usage --provider copilot --source api --format json); then
    printf 'copilot usage succeeded without a token\n' >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q '"provider":"copilot"'
printf '%s\n' "$output" | grep -q '"source":"api"'
printf '%s\n' "$output" | grep -q 'COPILOT_API_TOKEN'
