# -*- coding: utf-8 -*-
"""Extract the One-Seg tuner's 8051 firmware from Sony's vscd.sys.

The tuner module (054c:0279 in a Japanese VAIO P) is a Cypress EZ-USB with no
firmware of its own: out of reset it enumerates from an EEPROM that supplies
only Sony's VID and PID, and answers nothing but the boot loader's requests.
The firmware it needs is embedded in `vscd.sys`, the Windows driver for that
same hardware ID.

This script pulls it out into the record file `tools/upload_fw.cpp` replays.
Neither the driver nor the extracted image is committed to this repository -
they are Sony's - so this is how you regenerate the image from your own copy.

Where the driver comes from: the recovery discs for the machine. Their
payload is a set of `.MOD` packages, each a WIM image whose first 16 bytes are
XORed with the ASCII string "Sony Corporation"; clear that and the file opens
with any WIM tool. `vscd.sys` also appears in driver archives taken from a
VGN-P70H's `C:\\Windows\\Drivers`, under `INF/sonycxd/`.

The format, which the driver's own loader describes:

    .data + 0x10   start of the record table
    u16 length     1..16
    u16 address    where the bytes load in the 8051's memory
    u8  valid      zero; non-zero ends the table
    u8  data[16]   the payload, only `length` of which is used
    (stride is a fixed 22 bytes)

The loader at .text 0x111f2 sets its pointer to `.data+0x15` and reads the
length from -5 and the address from -3 of it, advancing 22 bytes at a time -
which is what fixes the table's start. Beginning at the wrong offset is not a
harmless error: the first fourteen records are the 8051's interrupt vectors
and the USB dispatch table, and firmware without them runs but cannot service
endpoint 0, leaving a device that answers nothing until it is power-cycled.

Usage:
  python3 recovery/extract_fw.py vscd.sys recovery/oneseg_fw.rec
  python3 recovery/extract_fw.py vscd.sys --image oneseg_fw.bin   # flat image
  python3 recovery/extract_fw.py --check oneseg_fw.rec            # validate

`install.sh` runs both of these for you: it finds a vscd.sys, extracts, checks
the result and installs it. Reach for this directly when that cannot find the
driver, or to inspect an image by hand.
"""

import struct
import sys


TABLE_OFFSET = 0x10
STRIDE = 22
MAX_RECORD = 16
# A firmware table is hundreds of records long. Anything shorter that survives
# the well-formedness test is a coincidence, not a table.
MIN_RECORDS = 32


def section(data, name, required=True):
    """Return (raw_offset, raw_size) of a PE section, or None if absent."""
    pe = struct.unpack('<I', data[0x3C:0x40])[0]
    count = struct.unpack('<H', data[pe + 6:pe + 8])[0]
    optional = struct.unpack('<H', data[pe + 20:pe + 22])[0]
    for i in range(count):
        entry = pe + 24 + optional + i * 40
        if data[entry:entry + 8].rstrip(b'\0') == name:
            _, _, raw_size, raw_offset = struct.unpack(
                '<IIII', data[entry + 8:entry + 24])
            return raw_offset, raw_size
    if required:
        raise SystemExit('no %s section - is this really vscd.sys?'
            % name.decode())
    return None


def read_run(blob, start):
    """Read consecutive well-formed records beginning at `start`.

    A record is well-formed when its valid byte is zero, its length is 1..16,
    and its address plus length stays inside the 8051's 16-bit space. Three
    conditions at a fixed 22-byte stride is a strict enough test that x86 code
    does not produce long runs of it by accident - which is the trap when the
    table is looked for as a bare 4-byte header.
    """
    out = []
    position = start
    while position + STRIDE <= len(blob):
        length, address = struct.unpack('<HH', blob[position:position + 4])
        # The loader's own terminator test is the valid byte, not the length.
        if blob[position + 4] != 0:
            break
        if length == 0 or length > MAX_RECORD:
            break
        if address + length > 0x10000:
            break
        out.append((address, blob[position + 5:position + 5 + length]))
        position += STRIDE
    return out


def looks_like_firmware(found):
    """Does this run assemble into a plausible 8051 image?

    The reset vector is the test: byte 0 of a real image is LJMP (0x02). A run
    of records that happens to validate but is not firmware almost never has
    one at address 0.
    """
    for address, payload in found:
        if address == 0 and len(payload) > 0:
            return payload[0] == 0x02
    return False


def records(data, verbose=True):
    """Find the record table, at the documented offset or anywhere else.

    The offset below is where the loader in the driver this was recovered from
    keeps its table. Other builds of vscd.sys have been reported that do not
    match it - the driver package for a given machine is not one fixed file -
    so when the documented offset yields nothing, every offset is tried and the
    longest well-formed run wins. That is why this does not simply fail: the
    firmware IS in the driver, and a missing table is nearly always the table
    having moved rather than not being there.
    """
    # A build without a .data section at all is not a reason to stop: the scan
    # below covers the whole file.
    where = section(data, b'.data', required=False)
    offset, size = where if where is not None else (0, 0)
    blob = data[offset:offset + size]

    found = read_run(blob, TABLE_OFFSET)
    if found and looks_like_firmware(found):
        return found

    if verbose:
        print('nothing at the documented offset (.data+0x%02x); scanning'
            % TABLE_OFFSET)

    # .data first, since that is where it has always been, then the whole file
    # in case this build put it in another section.
    for name, haystack, base in ((b'.data', blob, offset), (None, data, 0)):
        best = []
        best_start = None
        for start in range(0, max(0, len(haystack) - STRIDE)):
            run = read_run(haystack, start)
            if len(run) > len(best) and looks_like_firmware(run):
                best, best_start = run, start
        if len(best) >= MIN_RECORDS:
            if verbose:
                where = ('.data+0x%x' % best_start) if name \
                    else 'file offset 0x%x' % (base + best_start)
                print('found %d records at %s' % (len(best), where))
            return best

    return []


def assemble(found):
    """The records flattened into an address -> byte map."""
    image = {}
    for address, payload in found:
        for i, value in enumerate(payload):
            image[address + i] = value
    return image


def complaints(found, image):
    """Everything wrong with this image, as a list of one-line reasons.

    Empty means it is safe to upload. These are refusals rather than warnings
    when writing a .rec, because an image missing its vector table produces a
    module that runs the firmware and then answers nothing at all until it is
    power-cycled - the exact failure that is hardest to recognise from the
    outside.
    """
    problems = []
    if len(found) < MIN_RECORDS:
        problems.append('only %d records - a firmware table is hundreds long'
            % len(found))
    if image.get(0) != 0x02:
        problems.append('byte 0 is 0x%02x, not LJMP (0x02) - the parse is wrong'
            % image.get(0, 0xFF))
    if image.get(0x43) is None:
        problems.append('interrupt vector 0x43 (INT2/USB) is missing - the '
            'firmware would not service endpoint 0')
    return problems


def describe(found, image):
    total = sum(len(payload) for _, payload in found)
    low = min(address for address, _ in found)
    high = max(address + len(payload) for address, payload in found) - 1
    line = '%d records, %d bytes, loading to 0x%04x-0x%04x' \
        % (len(found), total, low, high)
    if image.get(0) == 0x02:
        line += ', reset vector LJMP 0x%04x' \
            % ((image.get(1, 0) << 8) | image.get(2, 0))
    return line, low, high


def read_records(path):
    """Parse a .rec file back into records - the inverse of what is written."""
    data = open(path, 'rb').read()
    found = []
    position = 0
    while position + 4 <= len(data):
        address, length = struct.unpack('<HH', data[position:position + 4])
        position += 4
        if length == 0:
            break
        if length > MAX_RECORD or position + length > len(data):
            raise SystemExit('%s: malformed record at offset %d'
                % (path, position - 4))
        found.append((address, data[position:position + length]))
        position += length
    if not found:
        raise SystemExit('%s: no records - the file is empty or truncated'
            % path)
    return found


def check(path):
    """Validate an already-extracted .rec, the way install.sh does."""
    found = read_records(path)
    image = assemble(found)
    line, _, _ = describe(found, image)
    problems = complaints(found, image)
    if problems:
        raise SystemExit('%s is not usable:\n  %s\n%s'
            % (path, '\n  '.join(problems), line))
    print(line)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == '--check':
        check(sys.argv[2])
        return

    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    data = open(sys.argv[1], 'rb').read()
    if data[:2] != b'MZ':
        raise SystemExit('%s is not a PE binary' % sys.argv[1])

    found = records(data)
    if not found:
        raise SystemExit(
            'no firmware records found in %s.\n'
            '\n'
            'What was looked for: records of "u16 length, u16 address, u8\n'
            'valid=0, u8 data[16]" at a fixed 22-byte stride, first at\n'
            '.data+0x%02x and then at every offset in the file. Note the\n'
            'length comes FIRST and the stride is 22, not 4 - a search for a\n'
            'bare 4-byte header finds x86 code instead and looks like a false\n'
            'positive table.\n'
            '\n'
            'If this really is vscd.sys for 054c:0279 and nothing was found,\n'
            'the build differs from the one this was recovered from. See\n'
            'FIRMWARE.md for what to do next.' % (sys.argv[1], TABLE_OFFSET))

    image = assemble(found)
    line, low, high = describe(found, image)
    print(line)

    if sys.argv[2] == '--image':
        # The flat dump is an analysis aid, so a suspect parse is reported and
        # still written - looking at it is how you work out what went wrong.
        for problem in complaints(found, image):
            print('warning: %s' % problem)
        flat = bytearray(0xFF for _ in range(high - low + 1))
        for address, value in image.items():
            flat[address - low] = value
        path = sys.argv[3] if len(sys.argv) > 3 else 'oneseg_fw.bin'
        open(path, 'wb').write(bytes(flat))
        print('wrote %s (base 0x%04x, %d bytes, gaps 0xff)'
            % (path, low, len(flat)))
        return

    # This one is going to be uploaded to hardware, so it is refused rather
    # than written with a warning nobody reads.
    problems = complaints(found, image)
    if problems:
        raise SystemExit('refusing to write %s:\n  %s\n\nSee FIRMWARE.md.'
            % (sys.argv[2], '\n  '.join(problems)))

    out = bytearray()
    for address, payload in found:
        out += struct.pack('<HH', address, len(payload)) + payload
    out += struct.pack('<HH', 0, 0)
    open(sys.argv[2], 'wb').write(bytes(out))
    print('wrote %s (%d bytes)' % (sys.argv[2], len(out)))


if __name__ == '__main__':
    main()
