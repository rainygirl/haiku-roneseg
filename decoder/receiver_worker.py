#!/usr/bin/env python3
"""Local ROneSeg decoder child. Binary pipes only; no sockets or SSH.

The native app owns the USB device. During initialization this process asks
for bounded vendor transfers; afterwards it accepts encrypted USB batches and
returns playable TS. Authentication uses private configuration and OpenSSL;
no vendor executable code is loaded.
"""
import argparse
from pathlib import Path
import struct
import sys

from native_session import ProtocolError, RecordedExchange, establish_session, load_auth
from transport import OneSegTransport


def read_exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.read(size - len(result))
        if not part:
            raise EOFError('decoder pipe closed')
        result.extend(part)
    return bytes(result)


class Wire:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer

    def send(self, kind, data=b''):
        self.writer.write(struct.pack('<cI', kind, len(data)) + data)
        self.writer.flush()

    def receive(self):
        kind, size = struct.unpack('<cI', read_exact(self.reader, 5))
        if size > 1024 * 1024:
            raise ProtocolError('decoder message too large')
        return kind, read_exact(self.reader, size)

    def control(self, request_type, request, value, index, length, payload=b''):
        if request_type not in (0x40, 0xc0) or request not in (0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x2b, 0x2c):
            raise ProtocolError('unexpected USB control request')
        if length > 64 or (request_type == 0x40 and len(payload) != length):
            raise ProtocolError('invalid control length')
        self.send(b'C', struct.pack('<BBHHH', request_type, request, value, index, length) + payload)
        kind, data = self.receive()
        if kind != b'C' or len(data) < 4:
            raise ProtocolError('invalid control response')
        result, = struct.unpack_from('<i', data)
        expected = length if request_type & 0x80 else 0
        if result != length or len(data) != 4 + expected:
            raise ProtocolError(f'USB control failed: {result}/{length}')
        return data[4:]


class UsbFrames:
    """Incremental framing, including headers split across USB reads."""
    def __init__(self):
        self.pending = bytearray()

    @staticmethod
    def header(data):
        return len(data) >= 16 and data[:2] == b'\x8c\xc4' and data[8:12] == bytes(4) and data[13:16] == bytes(3)

    def feed_frames(self, data):
        self.pending.extend(data)
        output = []
        cursor = 0
        # Keep the final frame until the next header establishes its boundary.
        while len(self.pending) - cursor >= 224:
            if self.header(self.pending[cursor:cursor + 16]) and self.header(self.pending[cursor + 208:cursor + 224]):
                output.append(bytes(self.pending[cursor:cursor + 208]))
                cursor += 208
            else:
                cursor += 1
        del self.pending[:cursor]
        return output

    def feed(self, data):
        return b''.join(frame[16:] for frame in self.feed_frames(data))


class BroadcastDecoder:
    def __init__(self, session):
        self.session = session
        self.frames = UsbFrames()
        self.transport = OneSegTransport()

    @staticmethod
    def pid(frame):
        return ((frame[6] & 31) << 8) | frame[7]

    def decode(self, frames):
        if not frames:
            return b''
        plain, _ = self.session.decrypt(b''.join(frame[16:] for frame in frames))
        if len(plain) != len(frames) * 188:
            raise ProtocolError('incorrect decrypted packet count')
        for i, frame in enumerate(frames):
            packet = plain[i * 188:(i + 1) * 188]
            if packet[0] != 0x47 or ((packet[1] & 31) << 8 | packet[2]) != self.pid(frame):
                raise ProtocolError('USB and decrypted transport headers disagree')
        return self.transport.feed(plain)

    def feed(self, data):
        frames = self.frames.feed_frames(data)
        output = b''
        if self.transport.pmt is None:
            # The clear USB header carries the TS PID. Discover the real PMT
            # before spending CPU on unsupported data-broadcast or null PIDs.
            tables = [f for f in frames if self.pid(f) in self.transport.sections]
            output = self.decode(tables)
            if self.transport.pmt is None:
                return output
            frames = [f for f in frames if self.pid(f) not in self.transport.sections]
        keep = self.transport.kept_pids | set(self.transport.sections)
        return output + self.decode([f for f in frames if self.pid(f) in keep])


def serve(auth_profile, wire, replay=None):
    profile = load_auth(auth_profile)
    wire.send(b'N', b'Authenticating tuner on VAIO...')
    # Initialization is before Tune: this operation resets the stream path.
    if replay:
        bridge = RecordedExchange(replay, profile)
        cipher = establish_session(bridge, profile, bridge.random_bytes, sleep=lambda _: None)
    else:
        cipher = establish_session(wire, profile)
    try:
        wire.send(b'N', b'Session keys ready on VAIO (native)')
        decoder = BroadcastDecoder(cipher)
        wire.send(b'R')
        while True:
            kind, data = wire.receive()
            if kind == b'Q' and not data:
                return
            if kind == b'R' and not data:
                decoder = BroadcastDecoder(cipher)
                wire.send(b'R')
            elif kind == b'D' and len(data) <= 4096 * 208:
                wire.send(b'T', decoder.feed(data))
            else:
                raise ProtocolError('unexpected decoder command')
    finally:
        cipher.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('auth_profile', type=Path)
    parser.add_argument('--replay', type=Path,
        help='offline protocol test using recorded USB replies; never receives RF')
    args = parser.parse_args()
    wire = Wire(sys.stdin.buffer, sys.stdout.buffer)
    try:
        serve(args.auth_profile, wire, args.replay)
    except EOFError:
        return 0
    except Exception as error:
        print(f'roneseg decoder: {error}', file=sys.stderr)
        wire.send(b'E', str(error).encode('utf-8')[:4096])
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
