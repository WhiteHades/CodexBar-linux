#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_dir=${CODEXBAR_DIST_DIR:-$repo/dist}
spec=$repo/packaging/rpm/codexbar-linux.spec
version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$repo/meson.build")
spec_version=$(sed -n 's/^Version:[[:space:]]*//p' "$spec")

"$repo/Scripts/require-release-tree.sh"
if test "$version" != "$spec_version"; then
    printf 'RPM spec version %s does not match Meson version %s\n' "$spec_version" "$version" >&2
    exit 1
fi
case "$(uname -m)" in
    x86_64|aarch64) ;;
    *) printf 'unsupported RPM architecture: %s\n' "$(uname -m)" >&2; exit 1 ;;
esac

archive=$("$repo/Scripts/source-archive.sh")
mkdir -p "$repo/.tmp" "$output_dir"
work=$(mktemp -d "$repo/.tmp/package-rpm.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
artifact_dir=$work/output
mkdir "$artifact_dir"

if command -v rpmbuild >/dev/null 2>&1; then
    topdir=$work/rpmbuild
    mkdir -p "$topdir/BUILD" "$topdir/BUILDROOT" "$topdir/RPMS" "$topdir/SOURCES" "$topdir/SPECS" "$topdir/SRPMS"
    cp "$archive" "$topdir/SOURCES/"
    cp "$spec" "$topdir/SPECS/"
    rpmbuild --define "_topdir $topdir" -ba "$topdir/SPECS/codexbar-linux.spec"
    find "$topdir/RPMS" "$topdir/SRPMS" -type f \( -name '*.rpm' -o -name '*.src.rpm' \) \
        -exec cp {} "$artifact_dir/" \;
else
    command -v docker >/dev/null 2>&1 || {
        printf '%s\n' 'RPM packaging requires rpmbuild or Docker' >&2
        exit 1
    }
    docker info >/dev/null 2>&1 || {
        printf '%s\n' 'Docker is installed but its daemon is unavailable' >&2
        exit 1
    }
    image=${CODEXBAR_FEDORA_IMAGE:-fedora:42}
    docker run --rm \
        -e HOST_UID="$(id -u)" \
        -e HOST_GID="$(id -g)" \
        -v "$archive:/sources/$(basename "$archive"):ro" \
        -v "$spec:/tmp/codexbar-linux.spec:ro" \
        -v "$artifact_dir:/out" \
        "$image" sh -eu -c '
            dnf -y install appstream dbus-daemon desktop-file-utils gcc meson ninja-build pkgconf-pkg-config \
                glib2-devel libcurl-devel json-c-devel sqlite-devel ncurses-devel rpm-build
            mkdir -p /tmp/rpmbuild/BUILD /tmp/rpmbuild/BUILDROOT /tmp/rpmbuild/RPMS \
                /tmp/rpmbuild/SOURCES /tmp/rpmbuild/SPECS /tmp/rpmbuild/SRPMS
            cp /sources/* /tmp/rpmbuild/SOURCES/
            cp /tmp/codexbar-linux.spec /tmp/rpmbuild/SPECS/
            rpmbuild --define "_topdir /tmp/rpmbuild" -ba /tmp/rpmbuild/SPECS/codexbar-linux.spec
            find /tmp/rpmbuild/RPMS /tmp/rpmbuild/SRPMS -type f -name "*.rpm" -exec cp {} /out/ \;
            rpm -qip /out/*.rpm >/dev/null
            chown "$HOST_UID:$HOST_GID" /out/*.rpm
        '
fi

set -- "$artifact_dir"/*.rpm
test -f "$1" || {
    printf '%s\n' 'rpmbuild completed without producing an RPM' >&2
    exit 1
}
for artifact do
    if command -v rpm >/dev/null 2>&1; then
        rpm -qip "$artifact" >/dev/null
    fi
    destination=$output_dir/$(basename "$artifact")
    cp "$artifact" "$destination"
    (
        cd "$output_dir"
        sha256sum "$(basename "$destination")" >"$(basename "$destination").sha256"
        sha256sum -c "$(basename "$destination").sha256"
    )
    printf '%s\n' "$destination"
done
