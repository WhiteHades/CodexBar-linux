#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")

if test "${CODEXBAR_VERIFIED_SOURCE_ARCHIVE:-0}" = 1 &&
    ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    exit 0
fi
tag_commit=$(git -C "$repo" rev-list -n 1 "refs/tags/v$version" 2>/dev/null || true)
head_commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)

if test -z "$tag_commit" || test "$tag_commit" != "$head_commit"; then
    printf 'release packaging refused: v%s must exist and identify HEAD\n' "$version" >&2
    exit 1
fi
if test -n "$(git -C "$repo" status --porcelain --untracked-files=all)"; then
    printf '%s\n' 'release packaging refused: commit or remove all tracked and untracked changes first' >&2
    exit 1
fi
