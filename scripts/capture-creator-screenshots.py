#!/usr/bin/env python3
"""Capture the current native Creator, never substitute an old image or mockup."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--executable", type=Path, default=ROOT / "build-pkg/bin/VLTONE.app/Contents/MacOS/VLTONE")
args = parser.parse_args()
binary = args.executable.resolve(strict=True)
output = ROOT / "web/public/images/creator"
output.mkdir(parents=True, exist_ok=True)
captured_at = datetime.now(timezone.utc)
evidence = ROOT / f"artifacts/creator-website-{captured_at.date().isoformat()}"
evidence.mkdir(parents=True, exist_ok=True)
manifest = {"capturedAt": captured_at.isoformat(), "executable": str(binary),
            "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "screenshots": []}
with tempfile.TemporaryDirectory(prefix="vlt-creator-site-") as directory:
    project = json.loads((ROOT / "resources/creator/Gentle Tremolo.vltcreator").read_text())
    # Keep the real working graph; only arrange it for a readable photograph.
    # The dry branch clears Gain, and Depth passes above the LFO card.
    project["positions"][""] = {"input": [30, 200], "gain": [650, 20],
                                "mix": [970, 200], "output": [1280, 200],
                                "interface": [30, 460], "lfo": [340, 720]}
    project_path = Path(directory) / "Gentle Tremolo.vltcreator"
    project_path.write_text(json.dumps(project, ensure_ascii=False, indent=2) + "\n")
    (evidence / "Gentle Tremolo.vltcreator").write_bytes(project_path.read_bytes())
    for locale in ("ru", "en"):
        staging = Path(directory) / locale
        staging.mkdir()
        env = {key: value for key, value in os.environ.items() if not key.startswith("DAW_CREATOR_")}
        env.update({"QT_QPA_PLATFORM": "offscreen:configfile=" + str(ROOT / "scripts/screenshot-screen.json"),
                    "QT_SCALE_FACTOR": "2", "DAW_PREF_DIR": str(staging / "preferences"),
                    "DAW_CREATOR_CHECK_DIR": str(staging),
                    "DAW_CREATOR_WEBSITE_PROJECT": str(project_path),
                    "VLT_GPU_WORKSPACE": "0"})
        result = subprocess.run([str(binary), "--creator-check", "--theme", "dark", "--language", locale],
                                cwd=binary.parent, env=env, capture_output=True, text=True, timeout=90)
        (evidence / f"capture-{locale}.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"Creator {locale} capture failed ({result.returncode}): {result.stderr[-2000:]}")
        source = staging / "creator.png"
        if not source.is_file():
            raise RuntimeError("Native capture did not produce creator.png")
        (evidence / f"editor-{locale}.png").write_bytes(source.read_bytes())
        for suffix, width in (("", 2880), ("-small", 1200)):
            target = output / f"editor-{locale}{suffix}.webp"
            subprocess.run(["cwebp", "-quiet", "-q", "88", "-resize", str(width), "0", str(source), "-o", str(target)], check=True)
            manifest["screenshots"].append({"path": str(target.relative_to(ROOT)), "sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "width": width})
        print(f"Captured fresh Creator {locale} from {binary.name}", flush=True)
(evidence / "capture.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
