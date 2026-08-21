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
# iconv is a real link dependency (ARIB text goes through EUC-JP), and on the
# gcc2 hybrid it is two packages: the primary one for the headers and the
# _x86_ one for the secondary architecture this actually builds for. Missing
# either fails as "iconv.h not found" or "cannot find -liconv", so say so here
# rather than leaving it to be worked out from the linker.
if ! $MAKE "$@"; then
	echo >&2
	echo "build failed. If it could not find iconv.h or -liconv:" >&2
	echo "  pkgman install libiconv_devel libiconv_x86_devel" >&2
	exit 1
fi

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
	FIRMWARE=yes
elif [ -f "$FWDIR/oneseg_fw.rec" ]; then
	echo "==> firmware already present at $FWDIR/oneseg_fw.rec"
	FIRMWARE=yes
else
	FIRMWARE=no
fi

# Applications: the user apps folder, and a Desktop launcher beside it.
# ~/config/apps is packagefs and read-only - a write there fails with
# "Read-only file system" - so user-built applications go to non-packaged.
APPS="$HOME/config/non-packaged/apps"
if ! mkdir -p "$APPS" 2>/dev/null; then
	echo "cannot create $APPS." >&2
	echo "Everything under ~/config except non-packaged/ and settings/ is a" >&2
	echo "read-only packagefs view; if non-packaged is not writable either," >&2
	echo "install by hand: cp $BINARY somewhere on a writable volume." >&2
	exit 1
fi
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

if [ "$FIRMWARE" = no ]; then
	echo
	echo "WARNING: no firmware image at $FWDIR/oneseg_fw.rec."
	echo "The module powers up blank, so until that file exists it exposes no"
	echo "streaming endpoint and a scan will find nothing - the hardware looks"
	echo "dead when it is not. Generate it from your own machine's driver:"
	echo
	echo "  python3 recovery/extract_fw.py vscd.sys $FWDIR/oneseg_fw.rec"
	echo
	echo "See FIRMWARE.md. Check with: ROneSeg --list-usb"
fi
