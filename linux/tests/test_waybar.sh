#!/bin/sh

set -eu

binary=$1
backend=$2
config=$3
output=$(CODEXBAR_BACKEND="$backend" "$binary" waybar)

case "$output" in
  *'"class":"critical"'*'"percentage":91'*) ;;
  *)
    printf 'unexpected Waybar output: %s\n' "$output" >&2
    exit 1
    ;;
esac

native_output=$(env -u CODEXBAR_BACKEND -u OPENROUTER_API_KEY -u OPENROUTER_API_URL \
  CODEXBAR_CONFIG="$config" "$binary" waybar)
case "$native_output" in
  *'OpenRouter API token is not configured'*) ;;
  *)
    printf 'unexpected native Waybar output: %s\n' "$native_output" >&2
    exit 1
    ;;
esac
