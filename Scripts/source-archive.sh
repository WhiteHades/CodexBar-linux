#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_dir=${CODEXBAR_DIST_DIR:-$repo/dist}
version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")

case "$version" in
    ''|*[!0-9A-Za-z._-]*) printf '%s\n' 'invalid Meson project version' >&2; exit 1 ;;
esac
if ! git -C "$repo" rev-parse --verify HEAD >/dev/null 2>&1; then
    printf '%s\n' 'source archives require a Git commit' >&2
    exit 1
fi
tag_commit=$(git -C "$repo" rev-list -n 1 "refs/tags/v$version" 2>/dev/null || true)
head_commit=$(git -C "$repo" rev-parse HEAD)
if test -z "$tag_commit" || test "$tag_commit" != "$head_commit"; then
    printf 'source archive refused: v%s must exist and identify HEAD\n' "$version" >&2
    exit 1
fi
if test -n "$(git -C "$repo" status --porcelain --untracked-files=all)"; then
    if test "${CODEXBAR_ALLOW_DIRTY:-0}" != 1; then
        printf '%s\n' 'source archive refused: commit or remove all tracked and untracked changes first' >&2
        printf '%s\n' 'set CODEXBAR_ALLOW_DIRTY=1 only for a non-release reproducibility check' >&2
        exit 1
    fi
    printf '%s\n' 'warning: creating a non-release reproducibility archive from a dirty tagged tree' >&2
fi

source_date_epoch=${SOURCE_DATE_EPOCH:-$(git -C "$repo" show -s --format=%ct HEAD)}
case "$source_date_epoch" in
    ''|*[!0-9]*) printf '%s\n' 'SOURCE_DATE_EPOCH must be an unsigned integer' >&2; exit 1 ;;
esac

mkdir -p "$repo/.tmp" "$output_dir"
work=$(mktemp -d "$repo/.tmp/source-archive.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
name=codexbar-linux-$version
file_list=$work/files
archive=$output_dir/$name.tar.gz

git -C "$repo" ls-files -z --cached --others --exclude-standard >"$file_list"
tar --create \
    --file="$work/$name.tar" \
    --directory="$repo" \
    --null \
    --files-from="$file_list" \
    --sort=name \
    --format=posix \
    --mtime="@$source_date_epoch" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --mode='u+rwX,go+rX,go-w' \
    --pax-option=delete=atime,delete=ctime \
    --transform="s,^,$name/,"
gzip -9 -n <"$work/$name.tar" >"$work/$name.tar.gz"
mv "$work/$name.tar.gz" "$archive"
(
    cd "$output_dir"
    sha256sum "$name.tar.gz" >"$name.tar.gz.sha256"
    sha256sum -c "$name.tar.gz.sha256" >&2
)
printf '%s\n' "$archive"
