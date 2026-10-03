"""Present a One-Seg service to generic TS players without re-encoding it.

The measured broadcast carries its PMT on PID 1fc8 but no PAT. Construct a
PAT from a CRC-checked PMT and retain its audio/video entries. Data-broadcast
tracks are not supported by R One-Seg and otherwise leave Media Kit probing
unknown codecs. Elementary packets and their PCR/PTS are never rewritten.
"""

import struct


def crc32_mpeg(data):
    crc = 0xffffffff
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    return crc


class Sections:
    def __init__(self):
        self.pending = bytearray()
        self.previous = None

    def drain(self):
        sections = []
        while len(self.pending) >= 3:
            if self.pending[0] == 0xff:
                self.pending.clear()
                break
            length = 3 + ((self.pending[1] & 15) << 8) + self.pending[2]
            if length < 12 or length > 4096:
                self.pending.clear()
                break
            if len(self.pending) < length:
                break
            section = bytes(self.pending[:length])
            del self.pending[:length]
            if crc32_mpeg(section) == 0:
                sections.append(section)
        return sections

    def feed(self, packet):
        if len(packet) != 188 or packet[0] != 0x47 or packet[1] & 0x80:
            self.pending.clear()
            self.previous = None
            return []
        if not packet[3] & 0x10:
            return []
        offset = 4
        if packet[3] & 0x20:
            offset += 1 + packet[4]
        if offset >= 188:
            return []
        counter = packet[3] & 15
        if self.previous is not None and counter != (self.previous + 1) % 16:
            self.pending.clear()
        self.previous = counter
        payload = packet[offset:]
        if packet[1] & 0x40:
            pointer = payload[0]
            if pointer + 1 > len(payload):
                self.pending.clear()
                return []
            sections = []
            if self.pending:
                self.pending.extend(payload[1:1 + pointer])
                sections = self.drain()
            self.pending = bytearray(payload[1 + pointer:])
            return sections + self.drain()
        if self.pending:
            self.pending.extend(payload)
            return self.drain()
        return []


def finish_section(section):
    length = len(section) - 3 + 4
    section[1] = (section[1] & 0xf0) | (length >> 8)
    section[2] = length & 255
    return bytes(section) + struct.pack('>I', crc32_mpeg(section))


def packetize(section, pid, counter):
    result = bytearray()
    remaining = b'\x00' + section
    first = True
    while remaining:
        result.extend(bytes((0x47, (pid >> 8) | (0x40 if first else 0),
            pid & 255, 0x10 | counter)))
        result.extend(remaining[:184].ljust(184, b'\xff'))
        remaining = remaining[184:]
        counter = (counter + 1) % 16
        first = False
    return bytes(result), counter


class OneSegTransport:
    def __init__(self):
        self.sections = {pid: Sections() for pid in (*range(0x1fc8, 0x1fd0), 0x11)}
        self.pending = bytearray()
        self.prelude = []
        self.pmt = None
        self.pmt_pid = None
        self.program = None
        self.transport_id = 1  # replaced as soon as the actual SDT arrives
        self.kept_pids = set()
        self.counters = [0, 0]
        self.until_tables = 0
        self.input_packets = 0
        self.transport_errors = 0

    def accept_pmt(self, section, pid):
        if section[0] != 2 or not section[5] & 1 or section[6:8] != b'\x00\x00':
            return
        if self.pmt_pid is not None and self.pmt_pid != pid:
            return
        info_length = ((section[10] & 15) << 8) | section[11]
        position = 12 + info_length
        if position > len(section) - 4:
            return
        filtered = bytearray(section[:position])
        pcr = ((section[8] & 31) << 8) | section[9]
        kept = {pcr, 0x11}
        tracks = 0
        while position + 5 <= len(section) - 4:
            size = 5 + ((section[position + 3] & 15) << 8) + section[position + 4]
            if position + size > len(section) - 4:
                return
            if section[position] in (0x1b, 0x0f, 0x11, 0x02, 0x03, 0x04):
                filtered.extend(section[position:position + size])
                kept.add(((section[position + 1] & 31) << 8) | section[position + 2])
                tracks += 1
            position += size
        if not tracks or position != len(section) - 4:
            return
        self.program = int.from_bytes(section[3:5], 'big')
        self.pmt_pid = pid
        self.kept_pids = kept
        pmt = finish_section(filtered)
        if pmt != self.pmt:
            self.until_tables = 0
        self.pmt = pmt

    def tables(self):
        pat = bytearray(b'\x00\xb0\x00' + struct.pack('>H', self.transport_id)
            + b'\xc1\x00\x00' + struct.pack('>HH', self.program, 0xe000 | self.pmt_pid))
        first, self.counters[0] = packetize(finish_section(pat), 0, self.counters[0])
        second, self.counters[1] = packetize(self.pmt, self.pmt_pid, self.counters[1])
        return first + second

    def emit(self, packets):
        result = bytearray()
        for packet in packets:
            pid = ((packet[1] & 31) << 8) | packet[2]
            if pid not in self.kept_pids or packet[1] & 0x80:
                continue
            if self.until_tables <= 0:
                result.extend(self.tables())
                self.until_tables = 40
            result.extend(packet)
            self.until_tables -= 1
        return bytes(result)

    def feed(self, data):
        self.pending.extend(data)
        packets = []
        while len(self.pending) >= 188:
            packet = bytes(self.pending[:188])
            del self.pending[:188]
            if packet[0] != 0x47:
                raise ValueError('decrypted TS lost packet alignment')
            self.input_packets += 1
            self.transport_errors += bool(packet[1] & 0x80)
            pid = ((packet[1] & 31) << 8) | packet[2]
            if pid in self.sections:
                for section in self.sections[pid].feed(packet):
                    if pid == 0x11 and section[0] == 0x42:
                        actual = int.from_bytes(section[3:5], 'big')
                        if actual != self.transport_id:
                            self.transport_id = actual
                            self.until_tables = 0
                    elif pid != 0x11:
                        self.accept_pmt(section, pid)
            packets.append(packet)
        if self.pmt is None:
            self.prelude.extend(packets)
            if len(self.prelude) > 4096:
                self.prelude = self.prelude[-4096:]
            return b''
        output = self.emit(self.prelude + packets)
        self.prelude.clear()
        return output
