#!/usr/bin/env python3
"""Refresh real VLTONE UI screenshots used by the website."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SHOTS = {
    "workspace": {},
    "piano": {"DAW_SHOT_PIANOROLL": "selected"},
    "mixer": {"DAW_SHOT_MIXER": "420"},
    "pattern": {"DAW_SHOT_PATTERN": "editor"},
    "sampler": {"DAW_SHOT_SAMPLER": "sample"},
    "equalizer": {"DAW_SHOT_EQUALIZER": "1", "DAW_SHOT_DELAY": "900"},
    "plugins": {"DAW_SHOT_PLUGINS": "0"},
    "ai": {"DAW_SHOT_AI": "complete"},
    "recovery": {"DAW_SHOT_RECOVERY": "1"},
}


def png_width(path: Path) -> int:
    with path.open("rb") as source:
        header = source.read(24)
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(f"Invalid PNG: {path}")
    return struct.unpack(">I", header[16:20])[0]


def run(command: list[str], **kwargs: object) -> None:
    result = subprocess.run(command, capture_output=True, text=True, check=False, **kwargs)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {' '.join(command[:3])}\n{result.stderr[-3000:]}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "build/bin/VLTONE")
    parser.add_argument("--only", action="append", choices=SHOTS.keys())
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    encoder = shutil.which("cwebp")
    if not encoder:
        parser.error("cwebp is required to create the website variants")
    chosen = {name: values for name, values in SHOTS.items() if not args.only or name in args.only}
    originals = ROOT / "artifacts/website-2026-09-26"
    public = ROOT / "web/public/images/studio"

    with tempfile.TemporaryDirectory(prefix="vltone-studio-shots-") as temp:
        staging = Path(temp)
        # The existing demo tone is an actual audio input for the sampler screenshot.
        sample = staging / "demo.wav"
        import math
        import wave
        with wave.open(str(sample), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(44100)
            output.writeframes(b"".join(struct.pack("<h", int(12000 * min(1, i / 1800, (35280 - i) / 5000) * math.sin(2 * math.pi * 220 * i / 44100))) for i in range(35280)))

        results: list[tuple[Path, Path]] = []
        for locale in ("ru", "en"):
            for name, options in chosen.items():
                source = staging / f"{name}-{locale}.png"
                prefs = staging / "prefs" / locale / name
                prefs.mkdir(parents=True)
                environment = {key: value for key, value in os.environ.items() if not key.startswith("DAW_SHOT_")}
                environment.update({
                    "QT_QPA_PLATFORM": "offscreen",
                    # QMessageBox clips translated button labels at 2x under
                    # Qt's offscreen platform; 1.5x keeps the real dialog legible.
                    "QT_SCALE_FACTOR": "1.5" if name == "recovery" else "2",
                    "DAW_PREF_DIR": str(prefs),
                    "DAW_SHOT_SIZE": "1600x1000",
                    **{key: str(sample) if value == "sample" else value for key, value in options.items()},
                })
                print(f"[{locale}] {name}", flush=True)
                run([str(executable), "--screenshot", str(source), "--theme", "logic", "--language", locale],
                    cwd=executable.parent, env=environment, timeout=30)
                width = png_width(source)
                if width < 400:
                    raise RuntimeError(f"Screenshot is unexpectedly narrow: {source} ({width}px)")
                results.append((source, originals / source.name))
                for mobile, limit in ((False, 2400), (True, 900)):
                    suffix = "-small" if mobile else ""
                    target = public / f"{name}-{locale}{suffix}.webp"
                    encoded = staging / target.name
                    run([encoder, "-quiet", "-q", "84", "-m", "6", "-resize", str(min(width, limit)), "0",
                         str(source), "-o", str(encoded)], timeout=30)
                    if encoded.stat().st_size <= 100:
                        raise RuntimeError(f"Failed to encode: {target}")
                    results.append((encoded, target))

        for source, target in results:
            target.parent.mkdir(parents=True, exist_ok=True)
            pending = target.with_name(target.name + ".new")
            shutil.copy2(source, pending)
            pending.replace(target)
        print(f"Published {len(chosen) * 2} source captures and {len(chosen) * 4} website WebP files", flush=True)


if __name__ == "__main__":
    main()
