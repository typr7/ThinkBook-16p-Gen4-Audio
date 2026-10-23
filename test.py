#!/usr/bin/env python3
"""Host regression tests; no amplifier or EFI writes."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='thinkbook-tests-') as temporary:
    for name in ['test_calibration','test_polling','output-policy']:
        exe=Path(temporary)/name
        subprocess.run(['clang++','-std=c++17','-include','initializer_list','-I'+str(root/'Audio'),str(root/'Tests'/(name+'.cpp')),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
for name in ['test_manual.py','test_preflight.py','check_stop_backport.py','test_build_inputs.py']:
    subprocess.run([sys.executable,str(root/'Tests'/name)],check=True)
print('All host regression tests passed. Reboot/hardware verification is separate.')
