#!/usr/bin/env python3
"""macOS review probes; use existing build libraries, never open an audio device."""
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
evidence = Path(__file__).resolve().parent
build = repo / "build"
work = Path(tempfile.mkdtemp(prefix="vlt-audio-review-"))
print(f"Evidence output: {work}", flush=True)
commands = subprocess.check_output(
    ["ninja", "-C", str(build), "-t", "commands", "controller_test"], text=True
).splitlines()
compile_args = shlex.split(next(
    c for c in commands if " -c " in c and c.endswith("/tests/controller_test.cpp")
))
for flag in ("-MT", "-MF"):
    if flag in compile_args:
        i = compile_args.index(flag)
        del compile_args[i:i + 2]
if "-MD" in compile_args:
    compile_args.remove("-MD")
link_args = shlex.split(next(
    c for c in reversed(commands) if " -o bin/controller_test " in c
).split("&&")[1])

# A private copy of the existing CLAP fixture deliberately spends 20 ms saving
# its state. This demonstrates the host's silent RenderGate, not vendor speed.
fixture = work / "SlowState.clap"
shutil.copytree(build / "plugins_test/DawTestGain.clap", fixture)
source = (repo / "tests/fixtures/test_clap/TestClapPlugin.cpp").read_text()
needle = "auto* self = TestPlugin::of(plugin);\n    const double values[4]"
assert needle in source
source = "#include <chrono>\n#include <thread>\n" + source.replace(
    needle,
    "std::this_thread::sleep_for(std::chrono::milliseconds(20));\n    " + needle,
)
(work / "SlowStateClap.cpp").write_text(source)
subprocess.run([
    "/usr/bin/c++", "-std=c++23", "-O2", "-shared", "-fPIC",
    "-I", str(repo / "third_party/clap/include"), str(work / "SlowStateClap.cpp"),
    "-o", str(fixture / "Contents/MacOS/DawTestGain"),
], check=True)

for name in ("review_probe", "device_probe", "slow_recovery_probe"):
    source = (evidence / f"{name}.cpp").read_text().replace(
        "/private/tmp/vlt-audio-review-2026-09-10", str(work)
    )
    (work / f"{name}.cpp").write_text(source)
    obj = str(work / f"{name}.o")
    compile_command = compile_args.copy()
    compile_command[compile_command.index("-o") + 1] = obj
    compile_command[-1] = str(work / f"{name}.cpp")
    subprocess.run(compile_command, cwd=build, check=True)
    link_command = link_args.copy()
    link_command[link_command.index("tests/CMakeFiles/controller_test.dir/controller_test.cpp.o")] = obj
    link_command[link_command.index("-o") + 1] = str(work / name)
    subprocess.run(link_command, cwd=build, check=True)
    with (work / f"{name}.txt").open("w") as log:
        subprocess.run([str(work / name)], stdout=log, stderr=subprocess.STDOUT,
                       timeout=60, check=True)
    print((work / f"{name}.txt").read_text(), flush=True)
