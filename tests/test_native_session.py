"""Native exchange tests with synthetic keys and independently encrypted replies."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from native_session import (PROFILE_MAGIC, ProtocolError, RecordedExchange,
    LeiraLink, establish_session, load_auth)

FIXTURE = Path(__file__).with_name('fixtures') / 'native_exchange.json'


class NativeSessionTests(unittest.TestCase):
    def setUp(self):
        self.record = json.loads(FIXTURE.read_text())
        self.profile = tuple(bytes.fromhex(self.record[k]) for k in ('client_id', 'auth_key'))

    def test_exact_register_sequence_and_independent_key_vector(self):
        bridge = RecordedExchange(FIXTURE, self.profile)
        cipher = establish_session(bridge, self.profile, bridge.random_bytes, sleep=lambda _: None)
        self.assertEqual(cipher.key, bytes.fromhex(self.record['packet_key']))
        self.assertEqual(len(bridge.transfers), len(self.record['transfers']))
        cipher.close()

    def test_corrupt_key_reply_is_rejected(self):
        bridge = RecordedExchange(FIXTURE, self.profile)
        bridge.recorded[91]['data'] = 'ffff'
        with self.assertRaisesRegex(ProtocolError, 'nonce verification'):
            establish_session(bridge, self.profile, bridge.random_bytes, sleep=lambda _: None)

    def test_wrong_profile_is_rejected_before_replay(self):
        with self.assertRaisesRegex(ProtocolError, 'does not match'):
            RecordedExchange(FIXTURE, (self.profile[0], bytes(16)))

    def test_tuner_authentication_rejection(self):
        bridge = RecordedExchange(FIXTURE, self.profile)
        bridge.recorded[56]['data'] = '4000'
        # The rejection path reads the error bit in a separate register read.
        bridge.recorded.insert(57, copy.deepcopy(bridge.recorded[56]))
        with self.assertRaisesRegex(ProtocolError, 'rejected authentication'):
            establish_session(bridge, self.profile, bridge.random_bytes, sleep=lambda _: None)

    def test_initialization_timeout_is_bounded(self):
        class Silent:
            reads = 0
            def control(self, kind, request, value, index, length, payload=b''):
                if kind == 0xc0:
                    self.reads += 1
                    return bytes(length)
                return b''
        bridge = Silent()
        with self.assertRaisesRegex(ProtocolError, 'initialization timed out'):
            LeiraLink(bridge, sleep=lambda _: None).initialize()
        self.assertEqual(bridge.reads, 1000)

    def test_profile_requires_magic_and_exact_length(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'auth.bin'
            valid = PROFILE_MAGIC + b''.join(self.profile)
            path.write_bytes(valid)
            self.assertEqual(load_auth(path), self.profile)
            for invalid in (valid[:-1], valid + b'\x00', b'badmagic' + valid[8:]):
                path.write_bytes(invalid)
                with self.assertRaises(ProtocolError):
                    load_auth(path)


if __name__ == '__main__':
    unittest.main()
