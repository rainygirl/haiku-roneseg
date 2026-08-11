#!/bin/sh
# Build and install R One-Seg on Haiku. Run this on the machine itself.
set -e

cd "$(dirname "$0")"

if [ "$(uname)" != "Haiku" ]; then
	echo "This builds against the Haiku SDK and only runs on Haiku." >&2
	exit 1
fi

# The 32-bit image this is meant for is the gcc2 hybrid, where the default
# g++ is GCC 2.95 and cannot build this code. setarch x86 selects the modern
# secondary compiler. On x86_64 there is no secondary arch and setarch is
# unnecessary, so only use it where it exists.
if [ "$(g++ -dumpversion | cut -d. -f1)" = "2" ]; then
	MAKE="setarch x86 make"
else
	MAKE="make"
fi

echo "==> building"
$MAKE "$@"

BINARY=$(find objects.* -maxdepth 1 -name ROneSeg -type f | head -n 1)
if [ -z "$BINARY" ]; then
	echo "build produced no binary" >&2
	exit 1
fi

# The module's firmware, where UsbTuner looks for it first. It is uploaded to
# the blank module automatically on the first tune.
FWDIR="$HOME/config/settings/roneseg"
mkdir -p "$FWDIR"
# The firmware is not shipped - you supply it, extracted from a driver you
# already have for the device. If a copy is present locally, install it.
if [ -f recovery/oneseg_fw.rec ]; then
	cp recovery/oneseg_fw.rec "$FWDIR/oneseg_fw.rec"
	echo "==> firmware installed to $FWDIR/oneseg_fw.rec"
fi

# Applications: the user apps folder, and a Desktop launcher beside it.
# ~/config/apps is packagefs and read-only - a write there fails with
# "Read-only file system" - so user-built applications go to non-packaged.
APPS="$HOME/config/non-packaged/apps"
mkdir -p "$APPS"
cp "$BINARY" "$APPS/ROneSeg"
mimeset -f "$APPS/ROneSeg"
ln -sf "$APPS/ROneSeg" "$HOME/Desktop/ROneSeg"

echo "==> installed to $APPS/ROneSeg and linked on the Desktop"
echo
echo "Usage:"
echo "  double-click ROneSeg (Desktop or ~/config/non-packaged/apps)"
echo "  then press Alt-S to scan - it walks the UHF channels, marks the ones"
echo "  a stream comes out of, and plays the first. Arrow keys + Enter retune."
echo "  Alt-U shows the USB report; --play capture.ts replays a file."
