#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${CODEXBAR_RELEASE_DIR:-$repo/.build/release}
output_dir=${CODEXBAR_DIST_DIR:-$repo/dist}
version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")
arch=$(uname -m)

"$repo/Scripts/require-release-tree.sh"

case "$version" in
    ''|*[!0-9A-Za-z._-]*) printf '%s\n' 'invalid Meson project version' >&2; exit 1 ;;
esac
case "$arch" in
    x86_64|aarch64) ;;
    *) printf 'unsupported release architecture: %s\n' "$arch" >&2; exit 1 ;;
esac

if test -f "$build_dir/build.ninja"; then
    meson setup --reconfigure "$build_dir" "$repo" --buildtype=release --wrap-mode=nodownload
else
    meson setup "$build_dir" "$repo" --buildtype=release --wrap-mode=nodownload
fi
meson compile -C "$build_dir"
meson test -C "$build_dir" --no-rebuild --print-errorlogs

mkdir -p "$repo/.tmp" "$output_dir"
stage=$(mktemp -d "$repo/.tmp/package.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
chmod 0755 "$stage"
DESTDIR="$stage" meson install -C "$build_dir" --strip
test -x "$stage/usr/local/bin/codexbar-linux"
test -x "$stage/usr/local/bin/codexbar-process-supervisor"
test -f "$stage/usr/local/share/applications/com.steipete.codexbar.desktop"
test -f "$stage/usr/local/share/metainfo/com.steipete.codexbar.metainfo.xml"
test -f "$stage/usr/local/share/icons/hicolor/scalable/apps/com.steipete.codexbar.svg"
test -f "$stage/etc/xdg/autostart/codexbar-status.desktop"
test "$("$stage/usr/local/bin/codexbar-linux" --version)" = "CodexBar $version"

name="codexbar-linux-$version-linux-$arch"
tar --owner=0 --group=0 --numeric-owner -C "$stage" -czf "$output_dir/$name.tar.gz" .
if ! tar --numeric-owner -tvzf "$output_dir/$name.tar.gz" |
    awk '$2 != "0/0" || ($1 ~ /^d/ && $1 != "drwxr-xr-x") { exit 1 }'; then
    printf '%s\n' 'archive contains unsafe ownership or directory metadata' >&2
    exit 1
fi
(
    cd "$output_dir"
    sha256sum "$name.tar.gz" >"$name.tar.gz.sha256"
    sha256sum -c "$name.tar.gz.sha256"
)
printf '%s\n' "$output_dir/$name.tar.gz"
