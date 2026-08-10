#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")

for command in appstreamcli bash desktop-file-validate xmllint; do
    command -v "$command" >/dev/null 2>&1 || {
        printf 'packaging metadata check requires %s\n' "$command" >&2
        exit 1
    }
done

for script in "$repo"/Scripts/*.sh "$repo/packaging/appimage/AppRun"; do
    sh -n "$script"
done
bash -n "$repo/packaging/aur/PKGBUILD.in"
xmllint --noout \
    "$repo/linux/data/com.steipete.codexbar.metainfo.xml" \
    "$repo/linux/data/icons/hicolor/scalable/apps/com.steipete.codexbar.svg"
desktop-file-validate \
    "$repo/linux/data/com.steipete.codexbar.desktop" \
    "$repo/linux/data/codexbar-status.desktop"
appstreamcli validate --no-net "$repo/linux/data/com.steipete.codexbar.metainfo.xml"

test "$(sed -n 's/^Version:[[:space:]]*//p' "$repo/packaging/rpm/codexbar-linux.spec")" = "$version"
test "$(xmllint --xpath 'string(/component/releases/release[1]/@version)' \
    "$repo/linux/data/com.steipete.codexbar.metainfo.xml")" = "$version"
test "$(sed -n 's/^## \([^ ]*\) .*/\1/p' "$repo/CHANGELOG.md" | sed -n '1p')" = "$version"
grep -Fq " - $version-1" "$repo/packaging/rpm/codexbar-linux.spec"
if grep -q 'SKIP' "$repo/packaging/aur/PKGBUILD.in"; then
    printf '%s\n' 'AUR template must never use a SKIP checksum' >&2
    exit 1
fi

if command -v rpmspec >/dev/null 2>&1; then
    rpmspec --parse "$repo/packaging/rpm/codexbar-linux.spec" >/dev/null
else
    printf '%s\n' 'rpmspec unavailable; RPM macro expansion check skipped'
fi
printf '%s\n' 'packaging metadata and script syntax are valid'
