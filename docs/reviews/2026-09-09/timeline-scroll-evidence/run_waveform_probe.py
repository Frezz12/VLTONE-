"""Build an isolated review probe; never modifies production sources/builds."""
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[3]
mode = sys.argv[1] if len(sys.argv) > 1 else 'ab'
if mode not in ('ab', 'baseline'):
    raise SystemExit('usage: run_waveform_probe.py [ab|baseline]')
flags = shlex.split(subprocess.check_output(
    ['pkg-config', '--cflags', '--libs', 'Qt6Gui', 'Qt6Widgets'], text=True))
with tempfile.TemporaryDirectory(prefix='vlt-scroll-review-') as scratch:
    binary = Path(scratch) / 'waveform-probe'
    command = ['clang++', '-std=c++23', '-O2',
               str(here / f'waveform-{mode}.cpp')]
    command += ['-I' + str(root / name)
                for name in ('app', 'controller', 'engine', 'core')]
    command += flags + ['-Wl,-rpath,/opt/homebrew/lib', '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
