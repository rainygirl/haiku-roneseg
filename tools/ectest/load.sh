#!/bin/sh
# Build and load the ectest test driver, or unload it again.
#
# Everything lands in ~/config/non-packaged, so nothing in the system image is
# touched and "unload" is a matter of deleting two files. The driver is
# read-only against the EC - see the comment at the top of ectest.c.
set -e

cd "$(dirname "$0")"

DRIVERS="$HOME/config/non-packaged/add-ons/kernel/drivers"
BIN="$DRIVERS/bin/ectest"
LINK="$DRIVERS/dev/misc/ectest"

if [ "$1" = "unload" ]; then
	rm -f "$LINK" "$BIN"
	echo "==> removed. /dev/misc/ectest disappears within a few seconds."
	exit 0
fi

if [ "$(uname)" != "Haiku" ]; then
	echo "This builds a Haiku kernel driver and only runs on Haiku." >&2
	exit 1
fi

# The gcc2 hybrid's default compiler cannot build this; the modern secondary
# one can. Same treatment the app's install.sh gives itself.
if [ "$(gcc -dumpversion | cut -d. -f1)" = "2" ]; then
	MAKE="setarch x86 make"
else
	MAKE="make"
fi

echo "==> building"
$MAKE

DRIVER=$(find objects.* -maxdepth 1 -name ectest -type f | head -n 1)
if [ -z "$DRIVER" ]; then
	echo "build produced no driver" >&2
	exit 1
fi

mkdir -p "$DRIVERS/bin" "$DRIVERS/dev/misc"
cp "$DRIVER" "$BIN"
ln -sf ../../bin/ectest "$LINK"

echo "==> loaded"
echo
echo "Read it with:   cat /dev/misc/ectest"
echo "Remove it with: $0 unload"
echo
echo "If /dev/misc/ectest does not appear, the driver refused to load -"
echo "check the syslog for lines beginning \"ectest:\"."
