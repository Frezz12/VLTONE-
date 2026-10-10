#!/usr/bin/env python3
"""Capture all marketing views of Night Bloom from VLTone's real interface."""

from __future__ import annotations

import argparse
from io import BytesIO
import os
from pathlib import Path
import subprocess

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
VIEWS = {
    "instrumental": ({}, ["showcase-instrumental", "showcase-arrangement", "workspace"]),
    "audio": ({"DAW_SHOT_SELECT": "Transitions", "DAW_SHOT_CLIP_EDITOR": "1"}, ["showcase-audio"]),
    "midi": ({"DAW_SHOT_PIANOROLL": "maximized", "DAW_SHOT_PIANOROLL_FIT": "1"}, ["showcase-midi"]),
    "mix": ({"DAW_SHOT_SIZE": "1800x1125", "DAW_SHOT_MIXER": "760", "DAW_SHOT_MIXER_WIDTH": "94", "DAW_SHOT_METERS": "1"}, ["showcase-mix", "mixer"]),
    "eq": ({"DAW_SHOT_EQUALIZER": "project"}, ["showcase-eq", "equalizer"]),
    "plugins": ({"DAW_SHOT_PLUGIN_SEARCH": "open", "DAW_SHOT_PLUGIN_SEARCH_QUERY": "Delay"}, ["plugins"]),
    "ai": ({"DAW_SHOT_AI": "complete", "DAW_SHOT_AI_PROJECT": "1", "DAW_SHOT_SCROLL": "100"}, ["ai"]),
    "recovery": ({"DAW_SHOT_RECOVERY": "1"}, ["recovery"]),
}


def encode(image: Image.Image) -> bytes:
    output = BytesIO()
    image.save(output, "WEBP", lossless=True, method=6)
    return output.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "build-windows/bin/VLTONE.exe")
    parser.add_argument("--project", type=Path, default=ROOT / "artifacts/site-redesign/Night Bloom/Night Bloom.vlt")
    parser.add_argument("--only", nargs="+", choices=VIEWS, default=list(VIEWS))
    parser.add_argument("--locales", nargs="+", choices=["ru", "en"], default=["ru", "en"])
    parser.add_argument("--publish", action="store_true", help="Copy reviewed captures to the site's public assets.")
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    project = args.project.resolve(strict=True)
    captures = ROOT / "artifacts/site-redesign/captures"
    captures.mkdir(parents=True, exist_ok=True)
    staged = captures / "website-assets"
    staged.mkdir(exist_ok=True)
    targets = []
    for locale in args.locales:
        for view in args.only:
            flags, names = VIEWS[view]
            preferences = captures / "prefs" / locale / view
            settings = preferences / "VLT Studio" / "VLT Studio Pro.ini"
            settings.parent.mkdir(parents=True, exist_ok=True)
            settings.write_text("[browser]\nvisible=false\n\n[ai]\nvisible=false\nwidth=520\n\n[contextPanel]\nfollowSelection=false\n", encoding="utf-8")
            environment = {key: value for key, value in os.environ.items() if not key.startswith("DAW_SHOT_")}
            environment.update({
                "QT_QPA_PLATFORM": "offscreen:configfile=" + os.path.relpath(ROOT / "scripts/screenshot-screen.json", executable.parent),
                "QT_SCALE_FACTOR": "2", "VLT_GPU_WORKSPACE": "0", "DAW_PREF_DIR": str(preferences),
                "DAW_SHOT_SIZE": "1440x900", "DAW_SHOT_MIXER": "off", "DAW_SHOT_SELECT": "Warm Keys",
                "DAW_SHOT_PLAYHEAD": str(60 / 108 * 44), "DAW_SHOT_DELAY": "1400",
                "DAW_SHOT_TRANSPORT_STYLE": "plain", "DAW_SHOT_REDUCE_MOTION": "1",
                "DAW_SHOT_DEMO_LOCALE": locale,
                **flags,
            })
            output = captures / f"{view}-{locale}.png"
            output.unlink(missing_ok=True)
            result = subprocess.run([str(executable), "--screenshot", str(output), "--theme", "dark", "--language", locale, str(project)],
                                    cwd=executable.parent, env=environment, capture_output=True, text=True, timeout=60)
            (captures / f"{view}-{locale}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode or not output.is_file():
                raise RuntimeError(f"Capture failed for {view}/{locale}: {result.stderr[-2000:]}")
            with Image.open(output) as screenshot:
                if view == "instrumental" and not any(color == (174, 212, 119) and count > 100
                                                       for count, color in screenshot.convert("RGB").getcolors(screenshot.width * screenshot.height) or []):
                    raise RuntimeError("The capture executable has the old theme; rebuild with the current Dark lime palette.")
                expected = tuple(int(dimension) * 2 for dimension in environment["DAW_SHOT_SIZE"].split("x"))
                if view != "recovery" and screenshot.size != expected:
                    raise RuntimeError(f"Unexpected capture size for {view}: {screenshot.size}")
                if min(screenshot.size) < 300:
                    raise RuntimeError(f"Capture too small for {view}: {screenshot.size}")
                encoded = encode(screenshot)
                for name in names:
                    target = staged / f"{name}-{locale}.webp"
                    target.write_bytes(encoded)
                    targets.append(target)
                    if not name.startswith("showcase-"):
                        small = screenshot.copy()
                        small.thumbnail((900, 900), Image.Resampling.LANCZOS)
                        target = staged / f"{name}-{locale}-small.webp"
                        target.write_bytes(encode(small))
                        targets.append(target)
                print(f"Captured {view}/{locale}: {screenshot.width}x{screenshot.height}, {len(encoded):,} bytes.", flush=True)
    if args.publish:
        for target in targets:
            (ROOT / "web/public/images/studio" / target.name).write_bytes(target.read_bytes())
        print(f"Updated {len(targets)} website assets; manual images are untouched.", flush=True)


if __name__ == "__main__":
    main()
