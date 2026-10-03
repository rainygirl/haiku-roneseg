#!/bin/sh
# Build and install the complete application using Haiku's package manager.
set -eu
cd "$(dirname "$0")"
[ "$(uname)" = Haiku ] || { echo "Run this on Haiku." >&2; exit 1; }
arch=$(getarch -p)
case "$arch" in
    x86_gcc2) devel=libiconv_x86_devel; crypto=openssl3_x86 ;;
    x86|x86_64) devel=libiconv_devel; crypto=openssl3 ;;
    *) echo "Unsupported architecture: $arch" >&2; exit 1 ;;
esac
pkgman install -y -R "$devel" "$crypto" cmd:python3
output=$(mktemp -d /tmp/roneseg-install.XXXXXX)
trap 'rm -rf "$output"' EXIT HUP INT TERM
sh packaging/build.sh "$output"
pkgman install -y -R "$output"/*.hpkg
ln -sf /boot/system/apps/ROneSeg "$(finddir B_DESKTOP_DIRECTORY)/ROneSeg"
echo "==> R One-Seg installed. Open it from Deskbar or the Desktop."
