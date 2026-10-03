"""Native AES for the measured CXD9192 chip-to-host packet format.

Each 192-byte ciphertext is AES-128-CBC with a fresh zero IV. Its plaintext
is a four-byte timestamp followed by a 188-byte TS packet. Keys are supplied
by the current hardware session, never stored here or derived from a capture.
OpenSSL's native EVP implementation does all block-cipher work.
"""
import ctypes as ct
from ctypes.util import find_library
from pathlib import Path
import sys


class Aes128Cbc:
    def __init__(self, key):
        if len(key) != 16:
            raise ValueError('link key must contain 16 bytes')
        # Haiku's gcc2 hybrid also has primary-architecture libraries. The
        # Python process uses the modern x86 ABI, so select that copy explicitly.
        if (sys.platform.startswith('haiku') and ct.sizeof(ct.c_void_p) == 4
                and Path('/boot/system/lib/x86/libcrypto.so.3').is_file()):
            path = '/boot/system/lib/x86/libcrypto.so.3'
        elif sys.platform == 'darwin':
            # Apple's system libcrypto deliberately aborts when loaded by
            # third-party callers. Use an installed OpenSSL for offline tests.
            path = next((str(p) for p in (Path('/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib'),
                Path('/usr/local/opt/openssl@3/lib/libcrypto.3.dylib')) if p.is_file()), None)
        elif sys.platform.startswith('haiku'):
            path = '/boot/system/lib/libcrypto.so.3'
        else:
            path = find_library('crypto')
        if not path:
            raise RuntimeError('OpenSSL libcrypto is required for native reception')
        self.lib = ct.CDLL(path)
        signatures = {
            'EVP_CIPHER_CTX_new': (ct.c_void_p, []),
            'EVP_CIPHER_CTX_free': (None, [ct.c_void_p]),
            'EVP_aes_128_cbc': (ct.c_void_p, []),
            'EVP_CipherInit_ex': (ct.c_int, [ct.c_void_p] * 5 + [ct.c_int]),
            'EVP_CIPHER_CTX_set_padding': (ct.c_int, [ct.c_void_p, ct.c_int]),
            'EVP_CipherUpdate': (ct.c_int, [ct.c_void_p, ct.c_void_p,
                ct.POINTER(ct.c_int), ct.c_void_p, ct.c_int]),
            'EVP_CipherFinal_ex': (ct.c_int, [ct.c_void_p, ct.c_void_p,
                ct.POINTER(ct.c_int)]),
        }
        for name, (result, args) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, args
        self.key = bytes(key)
        self.context = self.lib.EVP_CIPHER_CTX_new()
        if not self.context:
            raise MemoryError('could not allocate native AES context')

    def close(self):
        if getattr(self, 'context', None):
            self.lib.EVP_CIPHER_CTX_free(self.context)
            self.context = None
        self.key = b''

    def __del__(self):
        self.close()

    def crypt(self, data, *, encrypt=False):
        """AES-128-CBC with zero IV and no padding, reset on every call."""
        if not data or len(data) % 16 or len(data) > 4096 * 208:
            raise ValueError('expected complete AES blocks within the batch limit')
        if not self.context:
            raise RuntimeError('native AES context is closed')
        target = ct.create_string_buffer(len(data) + 16)
        size, tail = ct.c_int(), ct.c_int()
        lib, ctx = self.lib, self.context
        if (lib.EVP_CipherInit_ex(ctx, lib.EVP_aes_128_cbc(), None,
                self.key, bytes(16), int(encrypt)) != 1
                or lib.EVP_CIPHER_CTX_set_padding(ctx, 0) != 1
                or lib.EVP_CipherUpdate(ctx, target, ct.byref(size), data, len(data)) != 1
                or lib.EVP_CipherFinal_ex(ctx, ct.byref(target, size.value), ct.byref(tail)) != 1
                or size.value != len(data) or tail.value != 0):
            raise RuntimeError('native AES operation failed')
        return target.raw[:size.value]


class LinkCipher(Aes128Cbc):
    def decrypt(self, data):
        if not data or len(data) % 192 or len(data) > 4096 * 192:
            raise ValueError('expected 1..4096 complete encrypted packets')
        if not self.context:
            raise RuntimeError('native AES context is closed')
        # A zero ciphertext block between packets makes the next CBC block's
        # chaining value zero. Discard those separator outputs. This gives
        # independent packet decryption in one native call, including after
        # PID filtering or a lost USB packet.
        source = b''.join(data[i:i + 192] + bytes(16)
            for i in range(0, len(data), 192))
        plain = self.crypt(source)
        return (b''.join(plain[i + 4:i + 192] for i in range(0, len(plain), 208)),
            b''.join(plain[i:i + 4] for i in range(0, len(plain), 208)))
