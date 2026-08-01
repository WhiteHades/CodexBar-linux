#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${CODEXBAR_BUILD_DIR:-$repo/.build/check}

if test -f "$build_dir/build.ninja"; then
    meson setup --reconfigure "$build_dir" "$repo" --buildtype=debug --wrap-mode=nodownload
else
    meson setup "$build_dir" "$repo" --buildtype=debug --wrap-mode=nodownload
fi
meson compile -C "$build_dir"
meson test -C "$build_dir" --no-rebuild --print-errorlogs
"$repo/Scripts/verify-tree.sh"
git -C "$repo" diff --check
