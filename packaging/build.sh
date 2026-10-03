#!/bin/sh
# Build a complete hpkg on Haiku. Argument: output directory.
set -eu
cd "$(dirname "$0")/.."
[ "$(uname)" = Haiku ] || { echo "Run this on Haiku." >&2; exit 1; }
output=$1
mkdir -p "$output"
output=$(cd "$output" && pwd)
. ./packaging/roneseg.recipe
arch=$(getarch -p)
SETARCH=
name=roneseg
requires=$REQUIRES
if [ "$arch" = x86_gcc2 ]; then
    SETARCH="setarch x86"
    name=roneseg_x86
    requires=$REQUIRES_SECONDARY
fi
case "$arch" in x86_gcc2|x86|x86_64) ;; *) echo "Unsupported architecture: $arch" >&2; exit 1 ;; esac
BUILD
prefix=$(mktemp -d /tmp/roneseg-package.XXXXXX)
trap 'rm -rf "$prefix"' EXIT HUP INT TERM
INSTALL
cat > "$prefix/.PackageInfo" <<EOF
name $name
version $VERSION-$REVISION
architecture $arch
summary "$SUMMARY"
description "$DESCRIPTION"
packager "rainygirl <rainygirl@gmail.com>"
vendor "rainygirl"
copyrights { "$COPYRIGHT" }
licenses { "$LICENSE" }
provides {
    $name = $VERSION
    app:R_OneSeg = $VERSION
}
requires {
$requires
}
urls { "$HOMEPAGE" }
EOF
package create -q -z zlib -2 -C "$prefix" "$output/$name-$VERSION-$REVISION-$arch.hpkg"
echo "==> $output/$name-$VERSION-$REVISION-$arch.hpkg"
