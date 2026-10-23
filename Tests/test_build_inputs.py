#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Reject missing or changed OEM inputs before generating an embedded header."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('embed_firmware', ROOT / 'Tools/embed_firmware.py')
embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(embed)


class FirmwareInputs(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='thinkbook-inputs-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.inputs = self.root / 'firmware'
        self.inputs.mkdir()
        (self.root / 'Resources').mkdir()
        self.original_root = embed.ROOT
        embed.ROOT = self.root
        self.addCleanup(setattr, embed, 'ROOT', self.original_root)
        self.manifest = []
        for i, name in enumerate(['test.wmfw', 'left.bin', 'right.bin']):
            data = bytes(range(8 + i))
            (self.inputs / name).write_bytes(data)
            self.manifest.append({'name': name, 'bytes': len(data),
                                  'sha256': hashlib.sha256(data).hexdigest()})
        (self.root / 'Resources/OEM-firmware.json').write_text(json.dumps(self.manifest))
        self.header = self.root / 'generated/Devices/CS35L41/Resources/Firmware.hpp'

    def test_exact_inputs_generate_header(self):
        self.assertEqual(embed.generate(self.inputs, self.header), self.manifest)
        text = self.header.read_text()
        for symbol in ['tb_wmfw', 'tb_left', 'tb_right']:
            self.assertIn('static const uint8_t ' + symbol, text)
        self.assertIn('0x17AA,0x38A9', text)

    def test_missing_input_does_not_create_output(self):
        (self.inputs / 'right.bin').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing local OEM file'):
            embed.generate(self.inputs, self.header)
        self.assertFalse(self.header.exists())

    def test_changed_content_preserves_previous_header(self):
        embed.generate(self.inputs, self.header)
        original = self.header.read_bytes()
        (self.inputs / 'left.bin').write_bytes(bytes([255]) * 9)
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            embed.generate(self.inputs, self.header)
        self.assertEqual(self.header.read_bytes(), original)

    def test_truncated_input_is_rejected(self):
        (self.inputs / 'test.wmfw').write_bytes(bytes(2))
        with self.assertRaisesRegex(ValueError, 'size/SHA256 mismatch'):
            embed.generate(self.inputs, self.header)
        self.assertFalse(self.header.exists())


if __name__ == '__main__':
    unittest.main()
