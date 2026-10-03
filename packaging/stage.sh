#!/bin/sh
# Assemble the same payload for local installation and repository builds.
set -eu
cd "$(dirname "$0")/.."
prefix=$1
binary=$2
python3 -I -B packaging/check_payload.py data
mkdir -p "$prefix/apps" "$prefix/data/roneseg/decoder" \
    "$prefix/data/deskbar/menu/Applications" "$prefix/documentation/packages/roneseg/captures"
cp "$binary" "$prefix/apps/ROneSeg"
resattr -O -o "$prefix/apps/ROneSeg" "$prefix/apps/ROneSeg"
addattr -t mime BEOS:TYPE application/x-vnd.be-elfexecutable "$prefix/apps/ROneSeg"
for name in oneseg_fw.rec oneseg_demod.bin link-auth.bin; do
    cp "data/$name" "$prefix/data/roneseg/$name"
    chmod 644 "$prefix/data/roneseg/$name"
done
for name in native_session.py link_cipher.py transport.py receiver_worker.py run; do
    cp "decoder/$name" "$prefix/data/roneseg/decoder/$name"
done
chmod 755 "$prefix/data/roneseg/decoder/run"
ln -s ../../../../apps/ROneSeg "$prefix/data/deskbar/menu/Applications/R One-Seg"
cp README.md README.ko.md README.en.md README.pt-BR.md icon.png "$prefix/documentation/packages/roneseg/"
cp captures/vaio-oneseg-2026-10-03.png "$prefix/documentation/packages/roneseg/captures/"
