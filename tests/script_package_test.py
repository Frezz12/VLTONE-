"""Validate portable examples and hostile packages through the shipped headless CLI."""
import json
import copy
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

tool = pathlib.Path(sys.argv[1]).resolve()
examples = pathlib.Path(sys.argv[2]).resolve()

def check(*args, valid=True, cwd=None):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True, timeout=30, cwd=cwd)
    assert (result.returncode == 0) == valid, (args, result.stdout, result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix="vlt-sdk-") as directory:
    temporary = pathlib.Path(directory)
    packages = sorted(path.parent for path in examples.glob("*/manifest.json"))
    assert len(packages) >= 6
    for source in packages:
        check("--check", source)
        archive = temporary / (source.name + ".vltscript")
        check("--pack", source, archive)
        check("--check", archive)
    # The SDK's documented commands use paths relative to the current directory.
    shutil.copytree(packages[0], temporary / "relative")
    check("--check", "relative", cwd=temporary)
    check("--pack", "relative", "relative.vltscript", cwd=temporary)
    check("--check", "relative.vltscript", cwd=temporary)
    source = temporary / "ошибка.lua"
    source.write_text('error("Validation must not execute this")', encoding="utf-8")
    check("--check", source)
    source.write_text("local = invalid", encoding="utf-8")
    check("--check", source, valid=False)
    for bad_path in ("../escaped.lua", "/absolute.lua", "C:/escaped.lua", "CON.lua"):
        archive = temporary / "hostile.vltscript"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("manifest.json", (packages[0] / "manifest.json").read_bytes())
            output.writestr(bad_path, 'print("Never run")')
        check("--check", archive, valid=False)
    source = temporary / "bad-manifest"
    source.mkdir()
    (source / "main.lua").write_text("return true")
    manifest = json.loads((packages[0] / "manifest.json").read_text(encoding="utf-8"))
    for key, value in (("sdk", 999), ("sdk", 4294967297), ("description", {}),
                       ("id", "con"), ("permissions", ["project.read|project.write"])):
        invalid = copy.deepcopy(manifest)
        invalid[key] = value
        (source / "manifest.json").write_text(json.dumps(invalid), encoding="utf-8")
        check("--check", source, valid=False)
    invalid = copy.deepcopy(manifest)
    invalid["commands"][0]["contexts"] = ["clip|track"]
    (source / "manifest.json").write_text(json.dumps(invalid), encoding="utf-8")
    check("--check", source, valid=False)
    invalid = copy.deepcopy(manifest)
    invalid["commands"][0]["description"] = {}
    (source / "manifest.json").write_text(json.dumps(invalid), encoding="utf-8")
    check("--check", source, valid=False)

    document = {"format": "vltcreator", "version": 4, "kind": "lua",
                "manifest": manifest, "manifestSource": json.dumps(manifest),
                "sources": {command["entry"]: "return true" for command in manifest["commands"]},
                "resources": {}}
    project = temporary / "portable.vltcreator"
    for sources, resources in (({"extra.lua": "return 1", "EXTRA.lua": "return 2"}, {}),
                               ({"extra.lua": "return 1", "extra.lua/inner.lua": "return 2"}, {}),
                               ({}, {"resources/bad.bin": "invalid!base64"}),
                               ({}, {"resources/a.bin": "AA==", "resources/A.bin": "AQ=="})):
        invalid = copy.deepcopy(document)
        invalid["sources"].update(sources)
        invalid["resources"] = resources
        project.write_text(json.dumps(invalid), encoding="utf-8")
        check("--check", project, valid=False)
    # Resource contents (including .lua templates) are data, not executable source.
    document["resources"] = {"resources/template.lua": "bG9jYWwgPSBicm9rZW4="}
    document["sources"][".hidden.lua"] = "return true"
    project.write_text(json.dumps(document), encoding="utf-8")
    check("--check", project)
    portable = temporary / "resources.vltscript"
    check("--pack", project, portable)
    check("--check", portable)
    with zipfile.ZipFile(portable) as archive:
        assert archive.read("resources/template.lua") == b"local = broken"
        assert archive.read(".hidden.lua") == b"return true"
print("PASS: SDK examples, relative-path packaging, syntax-only checks, Unicode, archive paths, manifest types/SDK, portable names, base64")
