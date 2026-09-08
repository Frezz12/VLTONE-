"""Rebuild and run isolated diagnostic probes using the existing macOS Ninja build.

Run from any directory: python3 /absolute/path/to/run-render-review.py
Product sources and CMake configuration are not modified. Output goes to a new
temporary directory. The probe prints observed values; it is not a passing CI test.
"""
import pathlib
import shlex
import subprocess
import sys
import tempfile

if sys.platform != "darwin":
    raise SystemExit("This reproduction runner has only been validated on macOS.")
docs = pathlib.Path(__file__).resolve().parent
root = docs.parents[2]
build = root / "build"
out = pathlib.Path(tempfile.mkdtemp(prefix="vlt-render-review-"))
subprocess.run(["cmake", "--build", str(build), "--target", "render_safety_test",
                "daw_test_clap", "-j4"], check=True)
commands = subprocess.check_output(
    ["ninja", "-C", str(build), "-t", "commands", "render_safety_test"], text=True
).splitlines()
original = shlex.split(next(line for line in commands
                           if " -c " in line and "/tests/render_safety_test.cpp" in line))
compile_flags = []
skip = False
for token in original:
    if skip:
        skip = False
        continue
    if token in ("-MT", "-MF", "-o", "-c"):
        skip = True
        continue
    if token != "-MD":
        compile_flags.append(token)
subprocess.run(compile_flags + ["-o", str(out / "probe.o"), "-c",
                               str(docs / "render-review-probe.cpp")], cwd=build, check=True)
link = shlex.split(commands[-1])
if link[:2] != [":", "&&"] or link[-2:] != ["&&", ":"]:
    raise SystemExit("Unexpected Ninja link command; inspect it before adapting the runner.")
link = [str(out / "probe.o") if token.endswith("render_safety_test.cpp.o")
        else str(out / "probe") if token == "bin/render_safety_test"
        else token for token in link[2:-2]]
subprocess.run(link, cwd=build, check=True)
subprocess.run([original[0], "-std=c++23", "-O2", "-dynamiclib",
                "-I" + str(root / "third_party/clap/include"),
                str(docs / "render-review-plugin.cpp"), "-o", str(out / "Review.clap")], check=True)
result = subprocess.run([str(out / "probe"), str(out / "Review.clap"),
                         str(build / "plugins_test/DawTestGain.clap"), str(out / "audio")],
                        cwd=root, check=True, text=True, capture_output=True)
(out / "results.txt").write_text(result.stdout)
print(result.stdout, end="")
print("Artifacts:", out)
