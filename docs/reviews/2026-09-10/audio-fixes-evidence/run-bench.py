#!/usr/bin/env python3
"""Build/run the current pipeline benchmark with the configured Ninja toolchain.

No audio device is opened by default. --hardware runs the separate, silent
10-second device smoke probe instead; only diagnostic counters are saved.
The historical baseline is the pre-fix executable described in audio-fixes.md,
not the current implementations reached through the legacy setter names.
"""
from pathlib import Path
import argparse
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path)
mode = parser.add_mutually_exclusive_group()
mode.add_argument('--hardware', action='store_true')
mode.add_argument('--hardware-load', action='store_true')
args = parser.parse_args()
evidence = Path(__file__).resolve().parent
repo = evidence.parents[3]
build = args.build.resolve() if args.build else repo / 'build'
work = Path(tempfile.mkdtemp(prefix='vlt-audio-bench-'))
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'controller_test'], text=True).splitlines()
compile_args = shlex.split(next(c for c in commands if ' -c ' in c and c.endswith('/tests/controller_test.cpp')))
for flag in ('-MT', '-MF'):
    if flag in compile_args:
        index = compile_args.index(flag)
        del compile_args[index:index + 2]
if '-MD' in compile_args:
    compile_args.remove('-MD')
link_line = next(c for c in reversed(commands) if ' -o bin/controller_test ' in c)
link_args = shlex.split(link_line.split('&&')[1])
source = evidence / ('hardware-load-probe.cpp' if args.hardware_load else 'hardware-probe.cpp' if args.hardware else 'pipeline-bench.cpp')
obj, binary = work / 'probe.o', work / 'probe'
compile_args[compile_args.index('-o') + 1] = str(obj)
compile_args[-1] = str(source)
compile_args.insert(1, '-DATOMIC_ROUTING')
subprocess.run(compile_args, cwd=build, check=True)
link_args[link_args.index('tests/CMakeFiles/controller_test.dir/controller_test.cpp.o')] = str(obj)
link_args[link_args.index('-o') + 1] = str(binary)
subprocess.run(link_args, cwd=build, check=True)
with (work / 'output.txt').open('w') as output:
    subprocess.run([str(binary)], stdout=output, stderr=subprocess.STDOUT, timeout=120, check=True)
print((work / 'output.txt').read_text())
print(f'Output: {work}')
