"""Reject incomplete installation data before creating a package (no USB access)."""
import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'decoder'))
from native_session import load_auth
from link_cipher import Aes128Cbc


def check(directory):
    expected = {
        'oneseg_fw.rec': '866c14b9b4e81cd1a8083918bbb0a413c073381ab49678ebf15472f807d7c5d0',
        'oneseg_demod.bin': 'cf48f54b5e2c582001172de085c16971d2a4b3c5214544b3ea4f57dbdf92b036',
    }
    for name, digest in expected.items():
        if hashlib.sha256((directory / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Invalid receiver data: ' + name)
    load_auth(directory / 'link-auth.bin')
    cipher = Aes128Cbc(bytes(16))
    try:
        if cipher.crypt(bytes(16), encrypt=True).hex() != '66e94bd4ef8a2c3b884cfa59ca342b2e':
            raise ValueError('Native AES self-test failed')
    finally:
        cipher.close()
    print('Receiver data and native runtime verified.')


if __name__ == '__main__':
    check(Path(sys.argv[1]))
