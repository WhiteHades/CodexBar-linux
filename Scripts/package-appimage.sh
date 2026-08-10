#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${CODEXBAR_APPIMAGE_BUILD_DIR:-$repo/.build/package-appimage}
output_dir=${CODEXBAR_DIST_DIR:-$repo/dist}
version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")
linuxdeploy=${LINUXDEPLOY:-$(command -v linuxdeploy || true)}

"$repo/Scripts/require-release-tree.sh"
test -n "$linuxdeploy" || {
    printf '%s\n' 'linuxdeploy is required; install it and ensure it is in PATH or set LINUXDEPLOY' >&2
    exit 1
}
"$linuxdeploy" --list-plugins 2>/dev/null | grep -q '^appimage:' || {
    printf '%s\n' 'linuxdeploy output plugin "appimage" is not installed' >&2
    exit 1
}
case "$(uname -m)" in
    x86_64) appimage_arch=x86_64 ;;
    aarch64) appimage_arch=aarch64 ;;
    *) printf 'unsupported AppImage architecture: %s\n' "$(uname -m)" >&2; exit 1 ;;
esac

if test -f "$build_dir/build.ninja"; then
    meson setup --reconfigure "$build_dir" "$repo" --buildtype=release --wrap-mode=nodownload \
        --prefix=/usr --sysconfdir=/etc --localstatedir=/var
else
    meson setup "$build_dir" "$repo" --buildtype=release --wrap-mode=nodownload \
        --prefix=/usr --sysconfdir=/etc --localstatedir=/var
fi
meson compile -C "$build_dir"
meson test -C "$build_dir" --no-rebuild --print-errorlogs

mkdir -p "$repo/.tmp" "$output_dir"
work=$(mktemp -d "$repo/.tmp/package-appimage.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
appdir=$work/CodexBar.AppDir
DESTDIR="$appdir" meson install -C "$build_dir" --strip
test -x "$appdir/usr/bin/codexbar-linux"
test -x "$appdir/usr/bin/codexbar-process-supervisor"
rm "$appdir/etc/xdg/autostart/codexbar-status.desktop"
rmdir "$appdir/etc/xdg/autostart" "$appdir/etc/xdg" "$appdir/etc"
test ! -e "$appdir/etc/xdg/autostart/codexbar-status.desktop"

artifact=$output_dir/codexbar-linux-$version-$appimage_arch.AppImage
rm -f "$artifact"
ARCH=$appimage_arch OUTPUT="$artifact" "$linuxdeploy" \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/codexbar-linux" \
    --executable "$appdir/usr/bin/codexbar-process-supervisor" \
    --desktop-file "$appdir/usr/share/applications/com.steipete.codexbar.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/com.steipete.codexbar.svg" \
    --custom-apprun "$repo/packaging/appimage/AppRun" \
    --output appimage
test -x "$artifact"

mkdir "$work/extracted"
(
    cd "$work/extracted"
    "$artifact" --appimage-extract >/dev/null
)
test -x "$work/extracted/squashfs-root/usr/bin/codexbar-linux"
test -x "$work/extracted/squashfs-root/usr/bin/codexbar-process-supervisor"
test ! -e "$work/extracted/squashfs-root/etc/xdg/autostart/codexbar-status.desktop"
test "$(APPIMAGE_EXTRACT_AND_RUN=1 "$artifact" --version)" = "CodexBar $version"
(
    cd "$output_dir"
    sha256sum "$(basename "$artifact")" >"$(basename "$artifact").sha256"
    sha256sum -c "$(basename "$artifact").sha256"
)
printf '%s\n' "$artifact"
