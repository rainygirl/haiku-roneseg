import unittest

from transport import OneSegTransport, Sections, crc32_mpeg, finish_section, packetize


def pmt():
    # H.264 PID101, AAC PID102 and unsupported data PID103; PCR on 101.
    body = bytearray.fromhex('02b0001234c10000e101f000'
        '1be101f0000fe102f0000de103f000')
    return finish_section(body)


def payload(pid, counter=0):
    return bytes((0x47, pid >> 8, pid & 255, 0x10 | counter)) + bytes(184)


class TransportTests(unittest.TestCase):
    def test_sections_reassemble_and_check_crc(self):
        body = bytearray.fromhex('42f0001234c100001234ff') + bytes(300)
        section = finish_section(body)
        packets, _ = packetize(section, 17, 0)
        assembler = Sections()
        result = []
        for offset in range(0, len(packets), 188):
            result.extend(assembler.feed(packets[offset:offset + 188]))
        self.assertEqual(result, [section])
        corrupt = bytearray(packets)
        corrupt[30] ^= 1
        assembler = Sections()
        self.assertEqual(sum((assembler.feed(corrupt[i:i + 188])
            for i in range(0, len(corrupt), 188)), []), [])

    def test_cc_gap_discards_partial_section(self):
        section = finish_section(bytearray.fromhex('42f0001234c100001234ff') + bytes(300))
        packets, _ = packetize(section, 17, 0)
        assembler = Sections()
        self.assertEqual(assembler.feed(packets[:188]), [])
        changed = bytearray(packets[188:])
        changed[3] = 0x13
        self.assertEqual(assembler.feed(changed), [])

    def test_normalizer_preserves_av_and_drops_data_without_reencoding(self):
        table, _ = packetize(pmt(), 0x1fc8, 0)
        video, audio = payload(0x101), payload(0x102)
        raw = table + video + audio + payload(0x103)
        normalizer = OneSegTransport()
        output = b''.join(normalizer.feed(raw[i:i + 71]) for i in range(0, len(raw), 71))
        packets = [output[i:i + 188] for i in range(0, len(output), 188)]
        self.assertIn(video, packets)
        self.assertIn(audio, packets)
        self.assertNotIn(payload(0x103), packets)
        self.assertEqual(normalizer.program, 0x1234)
        pat = Sections().feed(packets[0])[0]
        self.assertEqual(pat[8:12], bytes.fromhex('1234ffc8'))
        self.assertEqual(crc32_mpeg(normalizer.pmt), 0)
        self.assertEqual(normalizer.pmt[12:-4], bytes.fromhex('1be101f0000fe102f000'))

    def test_bad_pmt_does_not_authorize_output(self):
        table, _ = packetize(pmt(), 0x1fc8, 0)
        damaged = bytearray(table)
        damaged[20] ^= 1
        self.assertEqual(OneSegTransport().feed(damaged + payload(0x101)), b'')


if __name__ == '__main__':
    unittest.main()
