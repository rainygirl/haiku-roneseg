"""CXD9192 link authentication without Windows code or an x86 emulator.

The owner's provisioned client identifier and authentication key are data,
not executable code. A fresh random pair is used for every live exchange.
Recovered protocol: host sends AES(0^64 | device_challenge | host_secret),
then requests a key with host_nonce | host_secret. The AES-decrypted reply
must contain host_nonce | packet_key. All AES messages use a zero CBC IV.
"""
import hmac
import json
import os
from pathlib import Path
import struct
import time

from link_cipher import Aes128Cbc, LinkCipher

PROFILE_MAGIC = b'R1AUTH01'


class ProtocolError(RuntimeError):
    pass


def load_auth(path):
    data = Path(path).read_bytes()
    if len(data) != 28 or data[:8] != PROFILE_MAGIC:
        raise ProtocolError('invalid local tuner authentication profile')
    return data[8:12], data[12:28]


class LeiraLink:
    def __init__(self, bridge, sleep=time.sleep):
        self.bridge = bridge
        self.sleep = sleep
        self.initialized = False

    def read(self, register):
        data = self.bridge.control(0xc0, 0x2b, 2, register, 2)
        if len(data) != 2:
            raise ProtocolError('short tuner register read')
        return int.from_bytes(data, 'little')

    def write(self, register, value):
        self.bridge.control(0x40, 0x2c, 2, register, 2, struct.pack('<H', value))

    def set_bits(self, register, bits):
        self.write(register, self.read(register) | bits)

    def pulse(self, bits):
        self.write(0x2a, self.read(0x2a) & (0xffff ^ bits))
        self.set_bits(0x2a, bits)

    def initialize(self):
        if self.initialized:
            return
        self.bridge.control(0x40, 0x20, 1, 0x7f0b, 1, b'\x01')
        for _ in range(1000):
            if self.read(0x2c) & 2:
                break
            self.sleep(0.001)
        else:
            raise ProtocolError('tuner link initialization timed out')
        self.write(0x1a, 0)
        self.write(0x1a, 0x8000)
        self.set_bits(0x22, 0x100)
        self.set_bits(0x24, 1)
        self.initialized = True

    def ready(self):
        if not self.read(0x2c) & 2:
            raise ProtocolError('tuner authentication link is not ready')

    def select_client(self, client_id):
        if len(client_id) != 4:
            raise ValueError('client identifier must contain four bytes')
        self.initialize()
        self.ready()
        self.write(6, 2)
        self.pulse(4)
        self.write_bytes(0x34, client_id)
        if self.read(0x2c) & 1:
            raise ProtocolError('tuner rejected the authentication client')
        if not self.read(6) & 2:
            raise ProtocolError('tuner did not acknowledge the authentication client')

    def write_bytes(self, register, data):
        if len(data) % 2:
            raise ValueError('tuner FIFO data must contain complete words')
        for i in range(0, len(data), 2):
            self.write(register, int.from_bytes(data[i:i + 2], 'big'))

    def read_bytes(self, register, length):
        return b''.join(self.read(register).to_bytes(2, 'big') for _ in range(length // 2))

    def challenge(self, client_id):
        self.select_client(client_id)
        self.ready()
        self.pulse(0x10)
        data = bytearray()
        for _ in range(2):
            self.pulse(0x200)
            data.extend(self.read_bytes(0x38, 4))
        return bytes(data)

    def authenticate(self, message):
        if len(message) != 32:
            raise ValueError('authentication message must contain 32 bytes')
        self.write(6, 0x10)
        self.pulse(0x20)
        self.write_bytes(0x2e, message)
        accepted = False
        for _ in range(10):
            if self.read(6) & 0x20:
                accepted = True
                break
            if self.read(6) & 0x40:
                raise ProtocolError('tuner rejected authentication')
            self.sleep(0.001)
        self.write(6, 0x20)
        if not accepted:
            raise ProtocolError('tuner authentication timed out')

    def key_reply(self, client_id, request):
        if len(request) != 32:
            raise ValueError('key request must contain 32 bytes')
        self.select_client(client_id)
        self.ready()
        self.pulse(0x40)
        self.write_bytes(0x2e, request)
        received = False
        for _ in range(10):
            if self.read(6) & 0x100:
                received = True
                break
            self.sleep(0.001)
        self.write(6, 0x100)
        if not received:
            raise ProtocolError('tuner key reply timed out')
        return self.read_bytes(0x30, 32)


def establish_session(bridge, profile, random_bytes=os.urandom, sleep=time.sleep):
    client_id, auth_key = profile
    cipher = Aes128Cbc(auth_key)
    try:
        secret, nonce = random_bytes(16), random_bytes(16)
        if len(secret) != 16 or len(nonce) != 16:
            raise ProtocolError('invalid authentication random source')
        link = LeiraLink(bridge, sleep)
        challenge = link.challenge(client_id)
        link.authenticate(cipher.crypt(bytes(8) + challenge + secret, encrypt=True))
        reply = link.key_reply(client_id, nonce + secret)
        plain = cipher.crypt(reply)
        if not hmac.compare_digest(plain[:16], nonce):
            raise ProtocolError('tuner key reply failed nonce verification')
        return LinkCipher(plain[16:])
    finally:
        cipher.close()


class RecordedExchange:
    """Strict offline replay; never opened by the normal receiver path."""
    def __init__(self, path, profile):
        self.recorded = json.loads(Path(path).read_text())['transfers']
        self.transfers = []
        # Host nonces must be identical when checking a saved exchange. FIFO
        # writes contain encrypted phase two followed by the clear key request.
        fifo = [bytes.fromhex(t['data'])[::-1] for t in self.recorded
                if t['type'] == 0x40 and t['request'] == 0x2c and t['index'] == 0x2e]
        if len(fifo) != 32 or any(len(word) != 2 for word in fifo):
            raise ProtocolError('recording lacks a complete authentication exchange')
        request = b''.join(fifo[16:])
        cipher = Aes128Cbc(profile[1])
        try:
            second = cipher.crypt(b''.join(fifo[:16]))
        finally:
            cipher.close()
        if second[:8] != bytes(8) or not hmac.compare_digest(second[16:], request[16:]):
            raise ProtocolError('recorded authentication does not match the profile')
        self.nonces = iter((request[16:], request[:16]))

    def random_bytes(self, size):
        if size != 16:
            raise ProtocolError('unexpected recorded nonce length')
        return next(self.nonces)

    def control(self, request_type, request, value, index, length, payload=b''):
        position = len(self.transfers)
        if position >= len(self.recorded):
            raise ProtocolError('replay exhausted')
        item = self.recorded[position]
        expected = tuple(item[k] for k in ('type', 'request', 'value', 'index', 'length'))
        if expected != (request_type, request, value, index, length):
            raise ProtocolError(f'replay request mismatch at {position}')
        data = bytes.fromhex(item['data'])
        if item['result'] != length or len(data) != length or (
                not request_type & 0x80 and data != payload):
            raise ProtocolError(f'replay payload/result mismatch at {position}')
        self.transfers.append(item)
        return data if request_type & 0x80 else b''
