"""Extract the One-Seg demodulator's DSP program from Sony's DtvCore.dll.

The demodulator in the VAIO P's tuner module (054c:0279) runs a 988-byte
program that the host uploads at every bring-up; without it the chip answers
on the bus but never locks. Sony's TV application carries it in DtvCore.dll
(module MODJ-134152 on recovery disc 1, an InstallShield bundle; the DLL is in
its data1.cab). It is Sony's code, so this repository carries only its
SHA-256 - this script finds it in your own copy.

  python3 recovery/extract_demod.py DtvCore.dll oneseg_demod.bin
  python3 recovery/extract_demod.py --check oneseg_demod.bin

R One-Seg looks for oneseg_demod.bin next to the firmware, in
~/config/settings/roneseg/.
"""

import hashlib
import sys

SIZE = 988
SHA256 = 'cf48f54b5e2c582001172de085c16971d2a4b3c5214544b3ea4f57dbdf92b036'
# Where it sits in the DtvCore.dll this was recovered from (file offset).
KNOWN_OFFSET = 0x1586e8


def matches(blob):
    return len(blob) == SIZE and hashlib.sha256(blob).hexdigest() == SHA256


def find(data):
    if matches(data[KNOWN_OFFSET:KNOWN_OFFSET + SIZE]):
        return KNOWN_OFFSET
    # Another build of the DLL: try every offset. The program starts with an
    # E8/E9 opcode byte, which keeps this to a few seconds.
    for offset in range(len(data) - SIZE + 1):
        if data[offset] in (0xe8, 0xe9) \
                and matches(data[offset:offset + SIZE]):
            return offset
    return None


def main(argv):
    if len(argv) == 3 and argv[1] == '--check':
        blob = open(argv[2], 'rb').read()
        print('OK' if matches(blob) else 'NOT the demodulator program')
        return 0 if matches(blob) else 1
    if len(argv) != 3:
        print(__doc__)
        return 2
    data = open(argv[1], 'rb').read()
    offset = find(data)
    if offset is None:
        print('the demodulator program is not in %s' % argv[1])
        return 1
    open(argv[2], 'wb').write(data[offset:offset + SIZE])
    print('wrote %s (%d bytes from offset 0x%x)' % (argv[2], SIZE, offset))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
