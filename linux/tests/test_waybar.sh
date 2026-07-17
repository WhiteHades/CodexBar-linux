#!/bin/sh

set -eu

binary=$1
backend=$2
output=$(CODEXBAR_BACKEND="$backend" "$binary" waybar)

case "$output" in
  *'"class":"critical"'*'"percentage":91'*) ;;
  *)
    printf 'unexpected Waybar output: %s\n' "$output" >&2
    exit 1
    ;;
esac
