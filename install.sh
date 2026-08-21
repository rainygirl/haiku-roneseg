#!/bin/sh
# Build and install R One-Seg on Haiku. Run this on the machine itself.
set -e

cd "$(dirname "$0")"

# --driver PATH points at the Windows driver to take the firmware from, for
# when it is somewhere this would not think to look. Everything else is passed
# through to make.
DRIVER=
if [ "$1" = "--driver" ]; then
	DRIVER="$2"
	shift 2
fi

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
EXTRACT="python3 recovery/extract_fw.py"

# Where a Windows driver might plausibly be sitting. The module needs the
# firmware image out of vscd.sys, and asking someone to run the extractor by
# hand is asking them to get the arguments right for a file they have never
# looked inside - so find it and do it here.
find_driver() {
	if [ -n "$DRIVER" ]; then
		echo "$DRIVER"
		return
	fi
	for dir in . "$HOME" "$HOME/Desktop" "$HOME/Downloads" "$HOME/driver" \
		"$HOME/drivers"; do
		[ -d "$dir" ] || continue
		# -iname: it comes off a Windows volume, so VSCD.SYS is as likely.
		found=$(find "$dir" -maxdepth 3 -iname 'vscd.sys' -type f 2>/dev/null \
			| head -n 1)
		if [ -n "$found" ]; then
			echo "$found"
			return
		fi
	done
}

install_firmware() {
	if [ -f recovery/oneseg_fw.rec ]; then
		cp recovery/oneseg_fw.rec "$FWDIR/oneseg_fw.rec"
		echo "==> firmware installed from recovery/oneseg_fw.rec"
		return 0
	fi
	if [ -f "$FWDIR/oneseg_fw.rec" ]; then
		return 0				# already there; validated below
	fi

	DRIVER_FILE=$(find_driver)
	if [ -z "$DRIVER_FILE" ]; then
		return 1
	fi

	if ! command -v python3 > /dev/null 2>&1; then
		echo "==> found $DRIVER_FILE but python3 is missing:" >&2
		echo "    pkgman install python3   then re-run ./install.sh" >&2
		return 1
	fi

	echo "==> extracting firmware from $DRIVER_FILE"
	# Into a temporary file first: a driver build this cannot parse must not
	# leave a half-written image where the app would try to upload it. The
	# extractor's own output is kept back unless it fails, since the summary
	# gets printed once below either way.
	TEMP="$FWDIR/oneseg_fw.rec.new"
	if OUTPUT=$($EXTRACT "$DRIVER_FILE" "$TEMP" 2>&1 \
		&& $EXTRACT --check "$TEMP" 2>&1); then
		mv "$TEMP" "$FWDIR/oneseg_fw.rec"
		return 0
	fi
	echo "$OUTPUT" >&2
	rm -f "$TEMP"
	return 1
}

if install_firmware; then
	FIRMWARE=yes
	# Say what is actually installed rather than that something is: a
	# truncated or wrong-driver image is the one failure that looks like
	# working hardware misbehaving.
	if command -v python3 > /dev/null 2>&1; then
		printf '==> firmware: '
		if ! $EXTRACT --check "$FWDIR/oneseg_fw.rec"; then
			FIRMWARE=no
		fi
	else
		echo "==> firmware: $FWDIR/oneseg_fw.rec (install python3 to verify it)"
	fi
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
	echo "WARNING: there is no firmware image, so the tuner will find nothing."
	echo "The module powers up blank: until it is given its firmware it exposes"
	echo "no streaming endpoint, and a scan comes back empty however healthy the"
	echo "hardware is."
	echo
	echo "Copy vscd.sys - the Windows driver for this machine's tuner - next to"
	echo "this script (or onto the Desktop) and run ./install.sh again. It will"
	echo "extract, check and install the image for you. If it is somewhere else:"
	echo
	echo "  ./install.sh --driver /path/to/vscd.sys"
	echo
	echo "Where to get vscd.sys, and what to do if it cannot be parsed:"
	echo "FIRMWARE.md"
fi
