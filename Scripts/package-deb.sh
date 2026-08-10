#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${CODEXBAR_DEB_BUILD_DIR:-$repo/.build/package-deb}
output_dir=${CODEXBAR_DIST_DIR:-$repo/dist}
version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")
maintainer='WhiteHades <WhiteHades@users.noreply.github.com>'

case "$version" in
    ''|*[!0-9A-Za-z.+~-]*) printf '%s\n' 'invalid Debian package version' >&2; exit 1 ;;
esac
if test -z "${SOURCE_DATE_EPOCH:-}"; then
    SOURCE_DATE_EPOCH=$(git -C "$repo" show -s --format=%ct HEAD 2>/dev/null || true)
fi
case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) printf '%s\n' 'Debian packaging requires a numeric SOURCE_DATE_EPOCH' >&2; exit 1 ;;
esac
export SOURCE_DATE_EPOCH
"$repo/Scripts/require-release-tree.sh"

build_package() {
    case "$(dpkg --print-architecture)" in
        amd64|arm64) architecture=$(dpkg --print-architecture) ;;
        *) printf 'unsupported Debian architecture: %s\n' "$(dpkg --print-architecture)" >&2; exit 1 ;;
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
    work=$(mktemp -d "$repo/.tmp/package-deb.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    stage=$work/root
    mkdir -p "$stage/DEBIAN"
    DESTDIR="$stage" meson install -C "$build_dir" --strip
    test -x "$stage/usr/bin/codexbar-linux"
    test -x "$stage/usr/bin/codexbar-process-supervisor"
    test -f "$stage/usr/share/applications/com.steipete.codexbar.desktop"
    test -f "$stage/usr/share/metainfo/com.steipete.codexbar.metainfo.xml"
    test -f "$stage/usr/share/icons/hicolor/scalable/apps/com.steipete.codexbar.svg"
    test -f "$stage/etc/xdg/autostart/codexbar-status.desktop"
    test "$("$stage/usr/bin/codexbar-linux" --version)" = "CodexBar $version"

    mkdir -p "$stage/usr/share/doc/codexbar-linux"
    gzip -9 -n <"$repo/CHANGELOG.md" >"$stage/usr/share/doc/codexbar-linux/changelog.gz"
    printf '%s\n' '/etc/xdg/autostart/codexbar-status.desktop' >"$stage/DEBIAN/conffiles"
    mkdir -p "$work/debian"
    cat >"$work/debian/control" <<EOF
Source: codexbar-linux
Section: utils
Priority: optional
Maintainer: $maintainer
Standards-Version: 4.7.2
Homepage: https://github.com/WhiteHades/CodexBar-linux

Package: codexbar-linux
Architecture: any
Description: native Linux AI usage monitor
 CodexBar displays AI provider usage and account limits in a terminal,
 desktop status item, Waybar, scripts, and JSON output.
EOF
    dependencies=$(
        cd "$work"
        dpkg-shlibdeps -O \
            -e"$stage/usr/bin/codexbar-linux" \
            -e"$stage/usr/bin/codexbar-process-supervisor"
    )
    dependencies=${dependencies#shlibs:Depends=}
    installed_size=$(du -sk "$stage" | cut -f1)
    cat >"$stage/DEBIAN/control" <<EOF
Package: codexbar-linux
Version: $version-1
Section: utils
Priority: optional
Architecture: $architecture
Maintainer: $maintainer
Installed-Size: $installed_size
Depends: $dependencies
Homepage: https://github.com/WhiteHades/CodexBar-linux
Description: native Linux AI usage monitor
 CodexBar displays AI provider usage and account limits in a terminal,
 desktop status item, Waybar, scripts, and JSON output.
EOF
    (
        cd "$stage"
        find etc usr -type f -print0 | sort -z | xargs -0 md5sum >DEBIAN/md5sums
    )
    artifact=$output_dir/codexbar-linux_${version}-1_${architecture}.deb
    dpkg-deb --root-owner-group --build "$stage" "$artifact"
    dpkg-deb --info "$artifact" >/dev/null
    dpkg-deb --contents "$artifact" | grep -q './usr/bin/codexbar-process-supervisor$'
    (
        cd "$output_dir"
        sha256sum "$(basename "$artifact")" >"$(basename "$artifact").sha256"
        sha256sum -c "$(basename "$artifact").sha256"
    )
    printf '%s\n' "$artifact"
}

if test "${CODEXBAR_DEB_CONTAINER:-0}" = 1; then
    build_package
    exit 0
fi

if test -r /etc/os-release; then
    . /etc/os-release
else
    ID=
    ID_LIKE=
fi
case " $ID ${ID_LIKE:-} " in
    *' debian '*|*' ubuntu '*) build_package ;;
    *)
        archive=$("$repo/Scripts/source-archive.sh")
        command -v docker >/dev/null 2>&1 || {
            printf '%s\n' 'Debian packaging requires Debian/Ubuntu or Docker' >&2
            exit 1
        }
        docker info >/dev/null 2>&1 || {
            printf '%s\n' 'Docker is installed but its daemon is unavailable' >&2
            exit 1
        }
        mkdir -p "$output_dir"
        image=${CODEXBAR_DEBIAN_IMAGE:-debian:trixie-slim}
        docker run --rm \
            -e CODEXBAR_DEB_CONTAINER=1 \
            -e CODEXBAR_VERIFIED_SOURCE_ARCHIVE=1 \
            -e CODEXBAR_DIST_DIR=/out \
            -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
            -e HOST_UID="$(id -u)" \
            -e HOST_GID="$(id -g)" \
            -v "$archive:/tmp/codexbar-source.tar.gz:ro" \
            -v "$output_dir:/out" \
            "$image" sh -eu -c '
                apt-get update
                DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
                    appstream build-essential ca-certificates curl dbus-daemon desktop-file-utils dpkg-dev \
                    libcurl4-openssl-dev libglib2.0-dev libjson-c-dev libncurses-dev \
                    libsqlite3-dev meson ninja-build pkg-config
                mkdir -p /work
                tar -xzf /tmp/codexbar-source.tar.gz -C /work
                source_dir=$(find /work -mindepth 1 -maxdepth 1 -type d -name "codexbar-linux-*" -print -quit)
                test -n "$source_dir"
                "$source_dir/Scripts/package-deb.sh"
                chown "$HOST_UID:$HOST_GID" /out/*.deb /out/*.deb.sha256
            '
        ;;
esac
