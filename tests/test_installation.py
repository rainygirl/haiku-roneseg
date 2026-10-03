"""Exercise the shipped launcher using only a fresh installation's files."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


class InstallationTests(unittest.TestCase):
    def test_isolated_launcher_with_no_settings_or_recovery_directory(self):
        root = Path(__file__).resolve().parents[1]
        fixture = root / 'tests/fixtures/native_exchange.json'
        record = json.loads(fixture.read_text())
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory) / 'data/roneseg'
            shutil.copytree(root / 'decoder', data / 'decoder')
            (data / 'link-auth.bin').write_bytes(b'R1AUTH01'
                + bytes.fromhex(record['client_id']) + bytes.fromhex(record['auth_key']))
            env = dict(os.environ, PYTHONPATH='/nonexistent', LIBUNICORN_PATH='/nonexistent')
            process = subprocess.run([str(data / 'decoder/run'), '--replay', str(fixture)],
                input=b'', capture_output=True, cwd=directory, env=env, timeout=15)
            self.assertEqual(process.returncode, 0, process.stderr.decode())
            output, kinds = process.stdout, []
            while output:
                self.assertGreaterEqual(len(output), 5)
                size = struct.unpack('<I', output[1:5])[0]
                self.assertGreaterEqual(len(output), 5 + size)
                kinds.append(output[:1])
                self.assertNotEqual(output[:1], b'E', output[5:5 + size])
                output = output[5 + size:]
            self.assertIn(b'R', kinds)


if __name__ == '__main__':
    unittest.main()
