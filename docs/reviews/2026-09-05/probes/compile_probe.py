from pathlib import Path
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[4]
source = Path(sys.argv[1]).resolve()
output = source.with_suffix('')
line = subprocess.check_output(['ninja', '-C', str(root/'build'), '-t', 'commands', 'sampler_test'], text=True).splitlines()[-1]
args = shlex.split(line)
args = args[args.index('/usr/bin/c++'):args.index('&&', 2)]
args = [str(root/'build'/a) if a.endswith(('.a','.o')) and not a.startswith('/') else a for a in args]
args[args.index(str(root/'build/tests/CMakeFiles/sampler_test.dir/sampler_test.cpp.o'))] = str(source)
args[args.index('-o')+1] = str(output)
args[1:1] = ['-std=c++23', '-I'+str(root), '-I'+str(root/'engine'), '-I'+str(root/'controller'), '-I'+str(root/'core'), '-I'+str(root/'plugins'), '-I/opt/homebrew/include']
subprocess.run(args, check=True)
