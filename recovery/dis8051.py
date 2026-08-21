# -*- coding: utf-8 -*-
"""A small 8051 disassembler, enough to read the One-Seg tuner's firmware.

The tuner's command set is not in Sony's Windows driver - that turned out to
be a plain pass-through, copying bRequest/wValue/wIndex straight from whatever
the application handed it. The authority on what those requests mean is the
firmware itself, which decodes them, so reading the firmware is the way to
learn the protocol.

This covers the opcodes that firmware actually uses rather than the whole
instruction set; anything unrecognised prints as a byte so the listing stays
aligned instead of silently desynchronising.

Usage:
  python3 recovery/dis8051.py oneseg_fw.bin 0x07f0 0x60
  python3 recovery/dis8051.py oneseg_fw.bin --setup     # the SETUPDAT handlers

The image is the flat one written by the extractor: base address 0x0000, gaps
filled with 0xff.
"""

import sys

# FX2 registers worth naming inline - a listing that says SETUPDAT is far
# easier to follow than one that says 0xE6B9.
FX2 = {
    0xE600: 'CPUCS', 0xE601: 'IFCONFIG', 0xE604: 'FIFORESET',
    0xE60B: 'EP2CFG', 0xE60C: 'EP4CFG', 0xE60D: 'EP6CFG', 0xE60E: 'EP8CFG',
    0xE612: 'EP2FIFOCFG', 0xE613: 'EP4FIFOCFG',
    0xE618: 'EP2AUTOINLENH', 0xE619: 'EP2AUTOINLENL',
    0xE65D: 'EP2FIFOBCH?', 0xE678: 'IOA/PORTACFG?',
    0xE680: 'IOA', 0xE681: 'IOB', 0xE682: 'IOC', 0xE683: 'IOD',
    0xE68A: 'OEA', 0xE68B: 'OEB', 0xE68C: 'OEC', 0xE68D: 'OED',
    0xE6A0: 'EP0CS?', 0xE6A1: 'EP1OUTBC', 0xE6A3: 'EP1INBC',
    0xE6B3: 'EP2CS', 0xE6B4: 'EP4CS',
    0xE6B8: 'SETUPDAT[0] bmRequestType',
    0xE6B9: 'SETUPDAT[1] bRequest',
    0xE6BA: 'SETUPDAT[2] wValueL',
    0xE6BB: 'SETUPDAT[3] wValueH',
    0xE6BC: 'SETUPDAT[4] wIndexL',
    0xE6BD: 'SETUPDAT[5] wIndexH',
    0xE6BE: 'SETUPDAT[6] wLengthL',
    0xE6BF: 'SETUPDAT[7] wLengthH',
    0xE740: 'EP0BUF',
}

BIT_SFR = {0xB0: 'P3', 0x90: 'P1', 0xA0: 'P2', 0x80: 'P0', 0xD0: 'PSW',
           0xA8: 'IE', 0xB8: 'IP', 0x98: 'SCON', 0x88: 'TCON'}


def sfr(a):
    names = {0x81: 'SP', 0x82: 'DPL', 0x83: 'DPH', 0x87: 'PCON', 0x88: 'TCON',
             0x89: 'TMOD', 0x8A: 'TL0', 0x8B: 'TL1', 0x8C: 'TH0', 0x8D: 'TH1',
             0x90: 'P1', 0x98: 'SCON', 0x99: 'SBUF', 0xA0: 'P2', 0xA8: 'IE',
             0xB0: 'P3', 0xB8: 'IP', 0xD0: 'PSW', 0xE0: 'ACC', 0xF0: 'B'}
    return names.get(a, '0x%02x' % a)


def dptr(v):
    name = FX2.get(v)
    return '#%04Xh%s' % (v, '  ; ' + name if name else '')


def disasm(image, base, address, count):
    """Yield (address, length, text) for count bytes starting at address."""
    end = address + count
    while address < end:
        def byte(offset=0):
            index = address - base + offset
            return image[index] if 0 <= index < len(image) else 0xFF

        op = byte()
        rel = lambda n: (address + n + (byte(n - 1) - 256
            if byte(n - 1) > 127 else byte(n - 1)))

        # Instructions are grouped by how their opcode is formed rather than
        # numerically, because that is how the 8051's encoding works.
        if op == 0x00:
            text, size = 'NOP', 1
        elif op == 0x02:
            text, size = 'LJMP  0x%04x' % (byte(1) << 8 | byte(2)), 3
        elif op == 0x12:
            text, size = 'LCALL 0x%04x' % (byte(1) << 8 | byte(2)), 3
        elif op == 0x22:
            text, size = 'RET', 1
        elif op == 0x32:
            text, size = 'RETI', 1
        elif op == 0x90:
            text, size = 'MOV   DPTR,%s' % dptr(byte(1) << 8 | byte(2)), 3
        elif op == 0xE0:
            text, size = 'MOVX  A,@DPTR', 1
        elif op == 0xF0:
            text, size = 'MOVX  @DPTR,A', 1
        elif op == 0xA3:
            text, size = 'INC   DPTR', 1
        elif op == 0x74:
            text, size = 'MOV   A,#0x%02x' % byte(1), 2
        elif op == 0x75:
            text, size = 'MOV   %s,#0x%02x' % (sfr(byte(1)), byte(2)), 3
        elif op == 0x85:
            text, size = 'MOV   %s,%s' % (sfr(byte(2)), sfr(byte(1))), 3
        elif op == 0xE5:
            text, size = 'MOV   A,%s' % sfr(byte(1)), 2
        elif op == 0xF5:
            text, size = 'MOV   %s,A' % sfr(byte(1)), 2
        elif op == 0xB4:
            text, size = 'CJNE  A,#0x%02x,0x%04x' % (byte(1), rel(3)), 3
        elif op == 0xB5:
            text, size = 'CJNE  A,%s,0x%04x' % (sfr(byte(1)), rel(3)), 3
        elif op == 0x80:
            text, size = 'SJMP  0x%04x' % rel(2), 2
        elif op == 0x60:
            text, size = 'JZ    0x%04x' % rel(2), 2
        elif op == 0x70:
            text, size = 'JNZ   0x%04x' % rel(2), 2
        elif op == 0x40:
            text, size = 'JC    0x%04x' % rel(2), 2
        elif op == 0x50:
            text, size = 'JNC   0x%04x' % rel(2), 2
        elif op == 0x20:
            text, size = 'JB    0x%02x.%d,0x%04x' % (byte(1) & 0xF8,
                byte(1) & 7, rel(3)), 3
        elif op == 0x30:
            text, size = 'JNB   0x%02x.%d,0x%04x' % (byte(1) & 0xF8,
                byte(1) & 7, rel(3)), 3
        elif op == 0xD2:
            text, size = 'SETB  0x%02x.%d' % (byte(1) & 0xF8, byte(1) & 7), 2
        elif op == 0xC2:
            text, size = 'CLR   0x%02x.%d' % (byte(1) & 0xF8, byte(1) & 7), 2
        elif op == 0xE4:
            text, size = 'CLR   A', 1
        elif op == 0xF4:
            text, size = 'CPL   A', 1
        elif op == 0xC3:
            text, size = 'CLR   C', 1
        elif op == 0xD3:
            text, size = 'SETB  C', 1
        elif op == 0x04:
            text, size = 'INC   A', 1
        elif op == 0x14:
            text, size = 'DEC   A', 1
        elif op == 0x24:
            text, size = 'ADD   A,#0x%02x' % byte(1), 2
        elif op == 0x25:
            text, size = 'ADD   A,%s' % sfr(byte(1)), 2
        elif op == 0x94:
            text, size = 'SUBB  A,#0x%02x' % byte(1), 2
        elif op == 0x54:
            text, size = 'ANL   A,#0x%02x' % byte(1), 2
        elif op == 0x44:
            text, size = 'ORL   A,#0x%02x' % byte(1), 2
        elif op == 0x64:
            text, size = 'XRL   A,#0x%02x' % byte(1), 2
        elif op == 0x53:
            text, size = 'ANL   %s,#0x%02x' % (sfr(byte(1)), byte(2)), 3
        elif op == 0x43:
            text, size = 'ORL   %s,#0x%02x' % (sfr(byte(1)), byte(2)), 3
        elif op == 0x23:
            text, size = 'RL    A', 1
        elif op == 0x03:
            text, size = 'RR    A', 1
        elif op == 0x33:
            text, size = 'RLC   A', 1
        elif op == 0x13:
            text, size = 'RRC   A', 1
        elif op == 0x93:
            text, size = 'MOVC  A,@A+DPTR', 1
        elif op == 0x83:
            text, size = 'MOVC  A,@A+PC', 1
        elif (op & 0x1F) == 0x01:            # AJMP page
            text, size = 'AJMP  0x%04x' % (((address + 2) & 0xF800)
                | ((op >> 5) << 8) | byte(1)), 2
        elif (op & 0x1F) == 0x11:            # ACALL page
            text, size = 'ACALL 0x%04x' % (((address + 2) & 0xF800)
                | ((op >> 5) << 8) | byte(1)), 2
        elif 0x78 <= op <= 0x7F:
            text, size = 'MOV   R%d,#0x%02x' % (op - 0x78, byte(1)), 2
        elif 0xE8 <= op <= 0xEF:
            text, size = 'MOV   A,R%d' % (op - 0xE8), 1
        elif 0xF8 <= op <= 0xFF:
            text, size = 'MOV   R%d,A' % (op - 0xF8), 1
        elif 0x08 <= op <= 0x0F:
            text, size = 'INC   R%d' % (op - 0x08), 1
        elif 0x18 <= op <= 0x1F:
            text, size = 'DEC   R%d' % (op - 0x18), 1
        elif 0x28 <= op <= 0x2F:
            text, size = 'ADD   A,R%d' % (op - 0x28), 1
        elif 0x98 <= op <= 0x9F:
            text, size = 'SUBB  A,R%d' % (op - 0x98), 1
        elif 0x88 <= op <= 0x8F:
            text, size = 'MOV   %s,R%d' % (sfr(byte(1)), op - 0x88), 2
        elif 0xA8 <= op <= 0xAF:
            text, size = 'MOV   R%d,%s' % (op - 0xA8, sfr(byte(1))), 2
        elif 0xB8 <= op <= 0xBF:
            text, size = 'CJNE  R%d,#0x%02x,0x%04x' % (op - 0xB8, byte(1),
                rel(3)), 3
        elif 0xD8 <= op <= 0xDF:
            text, size = 'DJNZ  R%d,0x%04x' % (op - 0xD8, rel(2)), 2
        elif op == 0xD5:
            text, size = 'DJNZ  %s,0x%04x' % (sfr(byte(1)), rel(3)), 3
        elif op == 0xE6 or op == 0xE7:
            text, size = 'MOV   A,@R%d' % (op - 0xE6), 1
        elif op == 0xF6 or op == 0xF7:
            text, size = 'MOV   @R%d,A' % (op - 0xF6), 1
        else:
            text, size = 'db    0x%02x' % op, 1

        yield address, size, text
        address += size


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    image = open(sys.argv[1], 'rb').read()
    base = 0x0000

    if len(sys.argv) > 2 and sys.argv[2] == '--setup':
        # Every place the firmware reads the USB setup packet, with a window
        # of code after it - that is where the command set is decided.
        regions = []
        for i in range(len(image) - 3):
            if (image[i] == 0x90 and image[i + 1] == 0xE6
                    and 0xB8 <= image[i + 2] <= 0xBF and image[i + 3] == 0xE0):
                regions.append(i)
        # Merge sites that are close together into one listing.
        merged = []
        for r in regions:
            if merged and r - merged[-1][1] < 0x40:
                merged[-1][1] = r + 0x30
            else:
                merged.append([max(0, r - 0x10), r + 0x30])
        for lo, hi in merged:
            print('; ---- 0x%04x .. 0x%04x ----' % (lo, hi))
            for a, n, text in disasm(image, base, lo, hi - lo):
                print('  %04x  %s' % (a, text))
            print()
        return

    address = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x40
    for a, n, text in disasm(image, base, address, count):
        raw = ' '.join('%02x' % image[a - base + i] for i in range(n))
        print('  %04x  %-9s %s' % (a, raw, text))


if __name__ == '__main__':
    main()
