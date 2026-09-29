#!/usr/bin/env python3
"""Capture both manual locales from the current VLTONE build, without touching user preferences."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave


ROOT = Path(__file__).resolve().parent.parent


def demo_wave(path: Path) -> None:
    rate = 44100
    count = int(rate * 0.8)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        frames = bytearray()
        for index in range(count):
            fade = min(1.0, index / 1800.0, (count - index) / 5000.0)
            frames.extend(struct.pack("<h", int(12000 * fade * math.sin(2 * math.pi * 220 * index / rate))))
        output.writeframes(frames)


def shots(sample_dir: Path, sample_path: Path) -> list[tuple[str, dict[str, str]]]:
    return [
        ("startup", {"DAW_SHOT_STARTUP": "1"}),
        ("arrangement", {}),
        ("track-folders", {"DAW_SHOT_TRACKS": "4", "DAW_SHOT_NEST": "3", "DAW_SHOT_SELECT": "Audio"}),
        ("cycle", {"DAW_SHOT_CYCLE": "2,8"}),
        ("context-panel", {"DAW_SHOT_CONTEXT": "0,1"}),
        ("browser-samples", {"DAW_SHOT_BROWSER": str(sample_dir), "DAW_SHOT_BROWSER_FILE": str(sample_path), "DAW_SHOT_DELAY": "1200"}),
        ("browser-plugins", {"DAW_SHOT_BROWSER_PLUGINS": "1"}),
        ("audio-editor", {"DAW_SHOT_CLIP_EDITOR": "1"}),
        ("recording", {"DAW_SHOT_RECORD": "1"}),
        ("takes", {"DAW_SHOT_TAKE": "layers", "DAW_SHOT_DELAY": "900"}),
        ("piano-roll", {"DAW_SHOT_PIANOROLL": "selected"}),
        ("pattern", {"DAW_SHOT_PATTERN": "editor"}),
        ("sampler", {"DAW_SHOT_SAMPLER": str(sample_path)}),
        ("equalizer", {"DAW_SHOT_EQUALIZER": "1", "DAW_SHOT_DELAY": "900"}),
        ("mixer", {"DAW_SHOT_MIXER": "420"}),
        ("plugin-picker", {"DAW_SHOT_MENU": "plugins"}),
        ("plugin-search", {"DAW_SHOT_PLUGIN_SEARCH": "open"}),
        ("plugin-manager", {"DAW_SHOT_PLUGINS": "0"}),
        ("plugin-paths", {"DAW_SHOT_PLUGINS": "1"}),
        ("plugin-blacklist", {"DAW_SHOT_PLUGINS": "2"}),
        ("automation-lane", {"DAW_SHOT_AUTOMATION": "1"}),
        ("automation-editor", {"DAW_SHOT_AUTOMATION_EDITOR": "select"}),
        ("ai-chat", {"DAW_SHOT_AI": "complete"}),
        ("ai-music", {"DAW_SHOT_AI": "music"}),
        ("notebook", {"DAW_SHOT_NOTEBOOK": "timed", "DAW_SHOT_DELAY": "900"}),
        ("web", {"DAW_SHOT_WEB": "1", "DAW_SHOT_DELAY": "1000"}),
        ("export-mix", {"DAW_SHOT_EXPORT": "mix"}),
        ("export-stems", {"DAW_SHOT_EXPORT": "stems"}),
        ("recovery", {"DAW_SHOT_RECOVERY": "1"}),
        ("settings-audio", {"DAW_SHOT_SETTINGS": "0"}),
        ("settings-transport", {"DAW_SHOT_SETTINGS": "2"}),
        ("settings-recording", {"DAW_SHOT_SETTINGS": "3"}),
        ("settings-context", {"DAW_SHOT_SETTINGS": "4"}),
        ("settings-browser", {"DAW_SHOT_SETTINGS": "5"}),
        ("settings-ai", {"DAW_SHOT_SETTINGS": "7"}),
        ("settings-account", {"DAW_SHOT_SETTINGS": "8"}),
        ("settings-language", {"DAW_SHOT_SETTINGS": "9"}),
        ("settings-recovery", {"DAW_SHOT_SETTINGS": "10"}),
        ("settings-themes", {"DAW_SHOT_SETTINGS": "11"}),
        ("settings-theme-editor", {"DAW_SHOT_SETTINGS": "12"}),
        ("settings-shortcuts", {"DAW_SHOT_SETTINGS": "13"}),
        ("settings-quick-import", {"DAW_SHOT_SETTINGS": "1"}),
        ("settings-notebook", {"DAW_SHOT_SETTINGS": "6"}),
        ("settings-interface", {"DAW_SHOT_SETTINGS": "14"}),
        ("settings-mixer", {"DAW_SHOT_SETTINGS": "15"}),
    ]


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as source:
        header = source.read(24)
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise RuntimeError(f"Invalid PNG: {path}")
    return struct.unpack(">II", header[16:24])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "build/bin/VLTONE")
    parser.add_argument("--output-root", type=Path, default=ROOT / "web/public/manual")
    parser.add_argument("--locale", choices=["ru", "en", "both"], default="both")
    parser.add_argument("--only", action="append", help="Capture just the named screenshot (repeatable)")
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    output_root = args.output_root.resolve()
    locales = ["ru", "en"] if args.locale == "both" else [args.locale]

    with tempfile.TemporaryDirectory(prefix="vltone-manual-shots-") as temp:
        temp_root = Path(temp)
        sample_dir = temp_root / "samples"
        sample_dir.mkdir()
        sample_path = sample_dir / "VLT Manual Tone.wav"
        demo_wave(sample_path)
        states = shots(sample_dir, sample_path)
        if args.only:
            unknown = set(args.only) - {name for name, _ in states}
            if unknown:
                parser.error(f"Unknown shot(s): {', '.join(sorted(unknown))}")
            states = [(name, values) for name, values in states if name in args.only]

        captured: list[tuple[Path, Path]] = []
        for locale in locales:
            for name, values in states:
                target = output_root / locale / f"{name}.png"
                staging = temp_root / locale / f"{name}.png"
                staging.parent.mkdir(exist_ok=True)
                prefs = temp_root / "prefs" / locale / name
                prefs.mkdir(parents=True)
                environment = {key: value for key, value in os.environ.items() if not key.startswith("DAW_SHOT_")}
                environment.update({
                    "QT_QPA_PLATFORM": "offscreen:configfile=" + os.path.relpath(ROOT / "scripts/screenshot-screen.json", executable.parent),
                    "VLT_GPU_WORKSPACE": "0",
                    "QT_SCALE_FACTOR": "1",
                    "DAW_PREF_DIR": str(prefs),
                    "DAW_SHOT_SIZE": "1440x1200",
                    **values,
                })
                print(f"[{locale}] {name}", flush=True)
                process = subprocess.run(
                    [str(executable), "--screenshot", str(staging), "--theme", "logic", "--language", locale],
                    cwd=executable.parent,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                )
                if process.returncode:
                    raise RuntimeError(f"{locale}/{name} exited {process.returncode}:\n{process.stderr[-3000:]}")
                if not staging.exists() or staging.stat().st_size <= 100:
                    raise RuntimeError(f"Screenshot was not created: {locale}/{name}\n{process.stderr[-3000:]}")
                width, height = png_dimensions(staging)
                if width < 200 or height < 150:
                    raise RuntimeError(f"Screenshot is unexpectedly small: {locale}/{name} ({width}x{height})")
                captured.append((staging, target))

        for staging, target in captured:
            target.parent.mkdir(parents=True, exist_ok=True)
            pending = target.with_name(target.name + ".new")
            shutil.copy2(staging, pending)
            pending.replace(target)
        print(f"Published {len(captured)} fresh screenshots in {output_root}", flush=True)


if __name__ == "__main__":
    main()
