#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=${1:-}
current_version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")

case "$version" in
    ''|*[!0-9A-Za-z._-]*)
        printf 'Usage: %s VERSION [OUTPUT_DIRECTORY]\n' "$0" >&2
        exit 2
        ;;
esac
if test "$version" != "$current_version"; then
    printf 'requested AUR version %s does not match Meson version %s\n' "$version" "$current_version" >&2
    exit 1
fi
tag_commit=$(git -C "$repo" rev-list -n 1 "refs/tags/v$version" 2>/dev/null || true)
head_commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)
if test -z "$tag_commit" || test "$tag_commit" != "$head_commit"; then
    printf 'AUR preparation refused: v%s must exist and identify HEAD\n' "$version" >&2
    exit 1
fi
command -v curl >/dev/null 2>&1 || {
    printf '%s\n' 'curl is required to verify release assets' >&2
    exit 1
}
command -v makepkg >/dev/null 2>&1 || {
    printf '%s\n' 'makepkg is required to generate .SRCINFO' >&2
    exit 1
}
local_archive=$("$repo/Scripts/source-archive.sh")

output_dir=${2:-${CODEXBAR_DIST_DIR:-$repo/dist}/aur/$version}
base_url=https://github.com/WhiteHades/CodexBar-linux/releases/download/v$version
archive=codexbar-linux-$version.tar.gz
checksum=$archive.sha256
mkdir -p "$repo/.tmp" "$output_dir"
work=$(mktemp -d "$repo/.tmp/prepare-aur.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

if ! curl --fail --silent --show-error --location "$base_url/$archive" --output "$work/$archive"; then
    printf 'AUR preparation refused: release asset is unavailable: %s/%s\n' "$base_url" "$archive" >&2
    exit 1
fi
if ! curl --fail --silent --show-error --location "$base_url/$checksum" --output "$work/$checksum"; then
    printf 'AUR preparation refused: release checksum is unavailable: %s/%s\n' "$base_url" "$checksum" >&2
    exit 1
fi
(
    cd "$work"
    sha256sum -c "$checksum"
)
checksum_lines=$(wc -l <"$work/$checksum")
checksum_name=$(awk 'NR == 1 { print $2 }' "$work/$checksum")
if test "$checksum_lines" -ne 1 || test "$checksum_name" != "$archive"; then
    printf '%s\n' 'AUR preparation refused: release checksum must contain only the source archive basename' >&2
    exit 1
fi
if ! cmp -s "$local_archive" "$work/$archive"; then
    printf '%s\n' 'AUR preparation refused: release source does not match the local tagged archive' >&2
    exit 1
fi
source_sha256=$(sha256sum "$work/$archive" | cut -d' ' -f1)
sed -e "s/@VERSION@/$version/g" -e "s/@SOURCE_SHA256@/$source_sha256/g" \
    "$repo/packaging/aur/PKGBUILD.in" >"$work/PKGBUILD"
if grep -q '@VERSION@\|@SOURCE_SHA256@\|SKIP' "$work/PKGBUILD"; then
    printf '%s\n' 'AUR preparation produced unresolved or unsafe checksum metadata' >&2
    exit 1
fi
(
    cd "$work"
    makepkg --printsrcinfo >.SRCINFO
)
install -m 0644 "$work/PKGBUILD" "$output_dir/PKGBUILD"
install -m 0644 "$work/.SRCINFO" "$output_dir/.SRCINFO"
printf '%s\n' "$output_dir/PKGBUILD"
