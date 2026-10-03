import io
import struct
import unittest

from receiver_worker import UsbFrames, Wire, ProtocolError, BroadcastDecoder
from test_transport import pmt, payload
from transport import packetize


def frame(value):
    return bytes.fromhex('8cc40001020308810000000002000000') + bytes([value]) * 192


class WorkerTests(unittest.TestCase):
    def test_filters_by_verified_pmt_before_decryption(self):
        class Session:
            def __init__(self): self.seen = []
            def decrypt(self, data):
                packets = [data[i + 4:i + 192] for i in range(0, len(data), 192)]
                self.seen.extend(((p[1] & 31) << 8) | p[2] for p in packets)
                return b''.join(packets), bytes(4 * len(packets))
        def wrap(packet):
            header = bytearray.fromhex('8cc40001020308810000000002000000')
            header[6:8] = packet[1:3]
            return bytes(header) + bytes(4) + packet
        table, _ = packetize(pmt(), 0x1fc8, 0)
        session = Session()
        decoder = BroadcastDecoder(session)
        source = b''.join(wrap(p) for p in (table, payload(0x101), payload(0x103), payload(0x1fff), payload(0x102), table))
        output = decoder.feed(source)
        self.assertEqual(session.seen, [0x1fc8, 0x101, 0x102])
        self.assertIn(payload(0x101), output)
        self.assertIn(payload(0x102), output)

    def test_every_split_preserves_complete_packets(self):
        source = frame(1) + frame(2) + frame(3)
        for split in range(len(source)):
            reader = UsbFrames()
            result = reader.feed(source[:split]) + reader.feed(source[split:])
            self.assertEqual(result, bytes([1]) * 192 + bytes([2]) * 192)

    def test_corrupt_frame_resynchronizes(self):
        reader = UsbFrames()
        self.assertEqual(reader.feed(frame(9)[:90] + frame(1) + frame(2)), bytes([1]) * 192)
        self.assertEqual(reader.feed(frame(3)), bytes([2]) * 192)

    def test_partial_pipe_message_and_eof(self):
        wire = Wire(io.BytesIO(b'T' + struct.pack('<I', 3) + b'abc'), io.BytesIO())
        self.assertEqual(wire.receive(), (b'T', b'abc'))
        with self.assertRaises(EOFError):
            wire.receive()

    def test_failed_usb_transfer_is_not_accepted(self):
        wire = Wire(io.BytesIO(b'C' + struct.pack('<Ii', 4, -1)), io.BytesIO())
        with self.assertRaisesRegex(ProtocolError, 'USB control failed'):
            wire.control(0xc0, 0x21, 1, 0x6e00, 1)


if __name__ == '__main__':
    unittest.main()
