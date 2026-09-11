#!/usr/bin/env python3
"""Remove unrelated nested QML modules copied with QtQuick/QtQml by macdeployqt.

Uses Qt's own transitive import scan, including the Quick Controls style imports.
Directory imports without a module declaration and ordinary assets stay intact.
Only a deployed .app is modified; the Qt installation is never touched.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess


def prune(bundle, required):
    bundle = Path(bundle).resolve()
    root = bundle / "Contents/Resources/qml"
    if bundle.suffix != ".app" or not root.is_dir():
        raise ValueError("Expected a deployed .app with Contents/Resources/qml")
    if not {"QtQuick", "QtWebEngine", "QtMultimedia"}.issubset(required):
        raise ValueError("The import scan is missing required VLTONE modules")
    for module in required:
        if not (root / module / "qmldir").is_file():
            raise ValueError(f"Required deployed QML module is missing: {module}")
    removed = []
    for manifest in sorted(root.rglob("qmldir"), key=lambda p: len(p.parts), reverse=True):
        directory = manifest.parent
        relative = directory.relative_to(root).as_posix()
        if relative in required or not re.search(r"^module\s+", manifest.read_text(), re.M):
            continue
        # A retained descendant also retains its parent module.
        if any(name.startswith(relative + "/") for name in required):
            continue
        if not directory.resolve().is_relative_to(root.resolve()):
            raise ValueError(f"Refusing to follow a module outside the deployed tree: {directory}")
        shutil.rmtree(directory)
        removed.append(relative)
    # The desktop app uses Cocoa's input method. An unrequested virtual-keyboard
    # plugin can otherwise retain dependencies on the modules removed above.
    if "QtQuick/VirtualKeyboard" not in required:
        plugin = bundle / "Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"
        if plugin.is_file():
            plugin.unlink()
    return removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--qmake", default="qmake")
    args = parser.parse_args()
    query = lambda key: subprocess.check_output([args.qmake, "-query", key], text=True).strip()
    scanner = Path(query("QT_HOST_LIBEXECS")) / "qmlimportscanner"
    imports = json.loads(subprocess.check_output([
        str(scanner), "-rootPath", str(args.source.resolve()),
        "-importPath", query("QT_INSTALL_QML")], text=True))
    required = {entry["relativePath"] for entry in imports if entry.get("relativePath")}
    removed = prune(args.bundle, required)
    print(f"QML deployment: retained {len(required)} imported modules; removed {len(removed)} unrelated modules")
    for name in removed:
        print("  removed", name)


if __name__ == "__main__":
    main()
