#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Validate locally supplied OEM files and generate a private build-time header."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SYMBOLS = ('tb_wmfw', 'tb_left', 'tb_right')
FOOTER = """const FirmwareResource cs35l41Firmware[]={
 {0x17AA,0x38A9,0,"ThinkBook17AA38A9-normal.wmfw","ThinkBook17AA38A9-normal.bin",tb_wmfw,sizeof(tb_wmfw),tb_left,sizeof(tb_left),tb_right,sizeof(tb_right),false}
};
const size_t cs35l41FirmwareCount=sizeof(cs35l41Firmware)/sizeof(cs35l41Firmware[0]);
"""

def generate(firmware_dir, destination):
    manifest = json.loads((ROOT / 'Resources/OEM-firmware.json').read_text())
    arrays = []
    for symbol, item in zip(SYMBOLS, manifest):
        path = firmware_dir / item['name']
        if not path.is_file():
            raise ValueError('Missing local OEM file: ' + item['name'])
        data = path.read_bytes()
        if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
            raise ValueError('OEM file size/SHA256 mismatch: ' + item['name'])
        body = '\n'.join(','.join(f'0x{x:02x}' for x in data[i:i+24]) + ','
                         for i in range(0, len(data), 24))
        arrays.append('static const uint8_t ' + symbol + '[]={\n' + body + '\n};\n')
    content = ('#pragma once\n// Generated local OEM payloads; not covered by source licenses.\n'
               '#include <IOKit/IOLib.h>\n' + ''.join(arrays) + FOOTER)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(content)
    return manifest

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.firmware_dir.resolve(), args.out.resolve())
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
    print('Verified OEM header generated locally:', args.out)
