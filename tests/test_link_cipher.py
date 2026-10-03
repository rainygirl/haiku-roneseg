"""Independent fixed-vector checks for native packet AES (no Sony DLLs)."""
import unittest
from link_cipher import LinkCipher

# AES-128-CBC, key 00..0f, zero IV, plaintext 00..bf. Generated independently
# with openssl enc, not with the implementation under test.
CIPHERTEXT = bytes.fromhex(
    '0a940bb5416ef045f1c39458c653ea5a3cf456b4ca488aa383c79c98b34797cb'
    '7e163e30ea49d32152a51a08a10ec02d677ff4ca5dd4696e75981c71283799c2'
    'b0f4065c0babdecc33b711f88f594e98d34dbf2b6da0584c37cfe113048069c1'
    '5f5466b0a1c864136fab0f6396277e4cad84c2adf7090cfc643a180dde410ee7'
    'b6123b1779f2d2b82919939671374f5ea6393f202ec81bde5fb478c6b5d7e6aa'
    '0b894273ff05f7148a552a6879090efc2517d8e6a4fd08563b45deae7f1efa82'
)

class CipherTests(unittest.TestCase):
    def setUp(self):
        self.cipher = LinkCipher(bytes(range(16)))

    def tearDown(self):
        self.cipher.close()

    def test_timestamp_and_transport_bytes(self):
        self.assertEqual(self.cipher.decrypt(CIPHERTEXT),
            (bytes(range(4,192)), bytes(range(4))))

    def test_packets_and_calls_have_independent_zero_ivs(self):
        expected = bytes(range(4,192)) * 3, bytes(range(4)) * 3
        self.assertEqual(self.cipher.decrypt(CIPHERTEXT * 3), expected)
        self.assertEqual(self.cipher.decrypt(CIPHERTEXT * 3), expected)
        self.assertEqual(self.cipher.decrypt(CIPHERTEXT)[0], bytes(range(4,192)))

    def test_bad_lengths_and_closed_context(self):
        for data in (b'', bytes(191), bytes(193), bytes(192 * 4097)):
            with self.assertRaises(ValueError): self.cipher.decrypt(data)
        self.cipher.close()
        with self.assertRaises(RuntimeError): self.cipher.decrypt(CIPHERTEXT)

if __name__ == '__main__': unittest.main()
