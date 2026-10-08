#!/usr/bin/env python3
"""Create Night Bloom, a portable instrumental project for real website captures."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import uuid
import wave

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
RATE = 48000
BPM = 108
BEAT = 60 / BPM
BAR = BEAT * 4
RNG = np.random.default_rng(108)


def identity(name: str) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "vltone/night-bloom/" + name))


def write_wave(path: Path, samples: np.ndarray) -> None:
    if samples.ndim == 1:
        samples = np.column_stack((samples, samples))
    pcm = (np.clip(samples, -.98, .98) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm.tobytes())


def drum(kind: str) -> np.ndarray:
    duration = {"kick": .5, "clap": .3, "hat": .12, "perc": .28}[kind]
    time = np.arange(round(duration * RATE)) / RATE
    noise = RNG.normal(0, .25, len(time))
    if kind == "kick":
        phase = 2 * np.pi * (46 * time + 75 * .025 * (1 - np.exp(-time / .025)))
        return .78 * np.sin(phase) * np.exp(-time * 10) + noise * np.exp(-time * 130)
    if kind == "clap":
        high = np.concatenate(([0], np.diff(noise)))
        envelope = sum(np.exp(-np.maximum(0, time - start) * 75) * (time >= start) for start in [0, .012, .026])
        return .48 * high * envelope + .16 * np.sin(2 * np.pi * 180 * time) * np.exp(-time * 45)
    if kind == "hat":
        high = np.concatenate(([0], np.diff(noise)))
        return .4 * high * np.exp(-time * 65)
    return .52 * np.sin(2 * np.pi * (210 * time + 14 * .04 * (1 - np.exp(-time / .04)))) * np.exp(-time * 21)


def rhythm(kind: str, bars: int) -> np.ndarray:
    result = np.zeros(round(BAR * bars * RATE))
    for bar in range(bars):
        positions = {
            "kick": [0, 1, 2, 3, 3.75] if bar % 4 == 3 else [0, 1, 2, 3],
            "clap": [1, 3],
            "hat": [step * .5 + (.035 if step % 2 else 0) for step in range(8)],
            "perc": [.75, 1.5, 2.75, 3.5] if bar % 2 else [.75, 2.5, 3.25],
        }[kind]
        for index, beat in enumerate(positions):
            signal = drum(kind) * (1 if kind != "hat" else (.62 if index % 2 else 1))
            start = round((bar * 4 + beat) * BEAT * RATE)
            count = min(len(signal), len(result) - start)
            result[start:start + count] += signal[:count]
    return result


def instrument(kind: str) -> tuple[np.ndarray, int]:
    time = np.arange(RATE * 4) / RATE
    root = 36 if kind == "bass" else 60
    frequency = 440 * 2 ** ((root - 69) / 12)
    phase = 2 * np.pi * frequency * time
    if kind == "bass":
        signal = .65 * np.sin(phase) + .16 * np.sin(phase * 2) + .04 * np.sin(phase * 3)
    elif kind == "keys":
        signal = (np.sin(phase) + .32 * np.sin(phase * 2) + .15 * np.sin(phase * 3)) * np.exp(-time * 1.7) * .48
    elif kind == "arp":
        signal = (np.sin(phase) + .23 * np.sin(phase * 3)) * np.exp(-time * 5) * .62
    elif kind == "lead":
        signal = sum(np.sin(phase * harmonic) / harmonic ** 1.8 for harmonic in range(1, 7)) * .35
    elif kind == "pad":
        left = sum(np.sin(phase * harmonic * .999) / harmonic ** 2 for harmonic in range(1, 5)) * .32
        right = sum(np.sin(phase * harmonic * 1.001) / harmonic ** 2 for harmonic in range(1, 5)) * .32
        return np.column_stack((left, right)), root
    else:
        signal = (np.sin(phase) + .4 * np.sin(phase * 2.76) + .15 * np.sin(phase * 5.4)) * np.exp(-time * 3) * .45
    attack = np.minimum(1, time / .004)
    return signal * attack, root


def midi_notes(kind: str, variation: int = 0) -> list[dict]:
    chords = [[48, 55, 58, 63], [44, 51, 55, 60], [51, 58, 62, 67], [46, 53, 60, 62]]
    result = []

    def note(pitch: int, start: float, length: float, velocity: int) -> None:
        result.append({"id": identity(f"{kind}/{variation}/{len(result)}"), "pitch": pitch,
                       "startBeats": start, "lengthBeats": length, "velocity": velocity})

    for bar, chord in enumerate(chords):
        start = bar * 4
        if kind == "bass":
            for index, beat in enumerate([0, .75, 1.5, 2.5, 3.25]):
                note(chord[0] - 12 + (12 if index == 3 else 0), start + beat, .55 if index == 0 else .32, 91 - index * 5)
        elif kind == "keys":
            for beat, length in [(0, 1.8), (2.5, 1.1)]:
                for index, pitch in enumerate(chord):
                    note(pitch, start + beat + index * .015, length, 73 + index * 3)
        elif kind == "arp":
            for index in range(8):
                pitch = chord[[0, 2, 1, 3, 2, 1, 3, 1][(index + variation) % 8]] + 12
                note(pitch, start + index * .5, .32, 74 if index % 2 == 0 else 59)
        elif kind == "lead":
            phrase = [[75, 74, 70, 67], [72, 75, 79, 75], [79, 77, 75, 74], [74, 72, 70, 67]][bar]
            for index, pitch in enumerate(phrase):
                note(pitch + (0 if variation == 0 else -12), start + [.25, 1.25, 2, 3.25][index], [.65, .45, 1, .6][index], 82 - index * 4)
        elif kind == "pad":
            for pitch in chord[1:]:
                note(pitch, start, 3.85, 52)
        else:
            for index, beat in enumerate([.5, 1.75, 3]):
                note(chord[[3, 2, 1][index]] + 24, start + beat, .7, 55 + index * 5)
    return result


def create_project(destination: Path) -> Path:
    content = destination / "Content"
    state = destination / "State"
    content.mkdir(parents=True, exist_ok=True)
    state.mkdir(exist_ok=True)
    for kind in ["kick", "clap", "hat", "perc"]:
        write_wave(content / f"{kind}.wav", rhythm(kind, 4))
    time = np.arange(round(BAR * 4 * RATE)) / RATE
    noise = RNG.normal(0, .025, len(time))
    texture = noise * (.35 + .65 * np.sin(np.pi * time / BAR) ** 2)
    write_wave(content / "texture.wav", texture)
    time = np.arange(round(BAR * 2 * RATE)) / RATE
    rise = RNG.normal(0, .035, len(time)) * (time / time[-1]) ** 2
    rise += .045 * np.sin(2 * np.pi * (300 * time + 140 * time ** 2)) * (time / time[-1]) ** 3
    write_wave(content / "rise.wav", rise)

    tracks = []
    palette = {"coral": 0xD8877C, "gold": 0xD4B478, "teal": 0x7AB9B0, "lime": 0xB7D98B, "blue": 0x85ACE0, "lilac": 0xB4A0D8}

    def track(name: str, kind: str, color: int, parent: str = "", height: int = 44) -> dict:
        item = {"id": identity(name), "name": name, "kind": kind, "color": color,
                "height": height, "expandedHeight": height, "expanded": True, "automationExpanded": False,
                "parentId": parent, "volume": .72, "pan": 0, "inputEnabled": False, "clips": []}
        tracks.append(item)
        return item

    def clip(item: dict, name: str, bar: int, bars: int, kind: str, **extra: object) -> None:
        item["clips"].append({"id": identity(f"{item['name']}/{bar}/{name}"), "name": name,
                              "kind": kind, "color": item["color"], "startSeconds": bar * BAR,
                              "durationSeconds": bars * BAR, "gain": 1, "channels": 2, **extra})

    rhythm_folder = track("Rhythm", "folder", palette["coral"], height=28)
    rhythm_folder["summing"] = True
    rhythm_folder["volume"] = .85
    for name, sound, color, blocks in [
        ("Kick", "kick", "coral", [0, 4, 8, 12, 20, 24, 28]),
        ("Clap", "clap", "gold", [4, 8, 12, 20, 24, 28]),
        ("Closed Hats", "hat", "teal", [0, 4, 8, 12, 20, 24, 28]),
        ("Percussion", "perc", "coral", [4, 8, 12, 24, 28]),
        ("Air & Texture", "texture", "lilac", [0, 4, 8, 12, 16, 20, 24, 28]),
    ]:
        item = track(name, "audio", palette[color], rhythm_folder["id"])
        item["iconId"] = "builtin:drum-kit" if sound != "texture" else "builtin:synth"
        item["volume"] = {"kick": .8, "clap": .7, "hat": .45, "perc": .52, "texture": .32}[sound]
        item["pan"] = {"hat": .16, "perc": -.22, "texture": .08}.get(sound, 0)
        for bar in blocks:
            clip(item, {"kick": "Pulse", "clap": "Backbeat", "hat": "Offbeat", "perc": "Syncopation", "texture": "Air"}[sound], bar, 4, "audio", file=f"{sound}.wav")

    harmony_folder = track("Instruments", "folder", palette["lime"], height=28)
    for name, sound, color, height, blocks in [
        ("Sub Bass", "bass", "lime", 48, [4, 8, 12, 20, 24, 28]),
        ("Warm Keys", "keys", "lilac", 56, [0, 4, 8, 12, 16, 20, 24, 28]),
        ("Glass Arp", "arp", "blue", 48, [4, 8, 12, 20, 24, 28]),
        ("Lead Melody", "lead", "lime", 52, [8, 12, 24, 28]),
        ("Wide Pad", "pad", "teal", 48, [0, 4, 8, 12, 16, 20, 24, 28]),
        ("Piano Sparkle", "bell", "gold", 48, [8, 12, 24, 28]),
    ]:
        item = track(name, "instrument", palette[color], harmony_folder["id"], height)
        item["iconId"] = "builtin:piano" if sound in ["keys", "bell"] else "builtin:synth"
        item["volume"] = {"bass": .48, "keys": .4, "arp": .28, "lead": .32, "pad": .18, "bell": .22}[sound]
        item["pan"] = {"arp": -.18, "bell": .23}.get(sound, 0)
        sample, root = instrument(sound)
        write_wave(content / f"{sound}-C.wav", sample)
        parameters = {"rootnote": root, "amp.on": 1, "amp.att": .12 if sound == "pad" else .006,
                      "amp.dec": .35, "amp.sus": .7, "amp.rel": .4 if sound == "pad" else .14,
                      "loop.mode": 1 if sound in ["pad", "bass", "lead"] else 0,
                      "loop.start": .15, "loop.end": .75}
        (state / f"{sound}.json").write_text(json.dumps({"version": 1, "sample": f"{sound}-C.wav", "params": parameters}), encoding="utf-8")
        item["instrument"] = {"id": identity(name + "/sampler"), "name": "Sampler", "format": "internal",
                              "uid": "daw.sampler", "vendor": "VLTone", "stateFile": f"{sound}.json",
                              "parameters": [{"id": key, "value": value} for key, value in parameters.items()]}
        for bar in blocks:
            phrase = {"bass": "Low Motion", "keys": "Cm7 · Abmaj7 · Ebmaj7 · Bbadd9", "arp": "Glass Steps", "lead": "Night Bloom", "pad": "Soft Horizon", "bell": "Sparkle"}[sound]
            notes = midi_notes(sound, 1 if bar >= 24 else 0)
            # Stable note ids belong to each clip, even where a phrase repeats.
            for index, note in enumerate(notes):
                note["id"] = identity(f"{name}/{bar}/note/{index}")
            clip(item, phrase, bar, 4, "midi", notes=notes)

    effects = track("Transitions", "audio", palette["coral"])
    effects["volume"] = .5
    for bar in [6, 14, 22, 30]:
        clip(effects, "Lift", bar, 2, "audio", file="rise.wav", fadeInSeconds=.3, fadeOutSeconds=.12)

    def insert(item: dict, name: str, uid: str, parameters: dict) -> None:
        item.setdefault("inserts", []).append({
            "id": identity(item["name"] + "/" + uid), "name": name,
            "format": "internal", "uid": uid, "vendor": "VLTone",
            "parameters": [{"id": key, "value": value} for key, value in parameters.items()],
        })

    def equalizer(item: dict, bands: list[tuple[int, float, float, float]]) -> None:
        parameters = {}
        for index, (kind, frequency, gain, q) in enumerate(bands, 1):
            prefix = f"band.{index:02d}."
            parameters.update({prefix + "enabled": 1, prefix + "type": kind,
                               prefix + "frequency": frequency, prefix + "gain": gain,
                               prefix + "q": q, prefix + "slope": 1})
        insert(item, "VLTone Equalizer", "daw.equalizer", parameters)

    channels = {item["name"]: item for item in tracks}
    equalizer(channels["Kick"], [(3, 28, 0, .7), (0, 60, 2.5, 1), (0, 280, -3, 1.2)])
    equalizer(channels["Warm Keys"], [(3, 140, 0, .7), (0, 350, -2.2, 1.1),
                                       (0, 2200, 1.8, .85), (2, 7200, 1.4, .7)])
    insert(rhythm_folder, "Compressor", "daw.compressor", {
        "ratio": 2.5, "threshold": -18, "attack": 22, "release": 140, "knee": 6, "mix": 75,
    })
    delay = track("Bloom Delay", "bus", palette["blue"], height=28)
    delay["volume"] = .62
    insert(delay, "Classic Delay", "daw.delay", {
        "timeMode": 0, "division": 10, "mode": 1, "feedback": 28, "mix": 100,
        "character": 4, "characterAmount": 28, "lowCut": 240, "highCut": 6800,
    })
    chorus = track("Wide Chorus", "bus", palette["teal"], height=28)
    chorus["volume"] = .55
    insert(chorus, "Chorus", "daw.chorus", {
        "amount": .65, "rate": .22, "depth": .3, "softness": .65,
    })
    for name, bus, level in [
        ("Warm Keys", delay, .18), ("Glass Arp", delay, .16),
        ("Lead Melody", delay, .22), ("Piano Sparkle", delay, .28),
        ("Wide Pad", chorus, .24), ("Air & Texture", chorus, .1),
    ]:
        channels[name].setdefault("sends", []).append({
            "id": identity(name + "/send/" + bus["name"]),
            "destination": bus["id"], "level": level, "preFader": False, "enabled": True,
        })
    manifest = destination / (destination.name + ".vlt")
    project = {"format": "vlt-project", "version": 15, "name": "Night Bloom", "author": "VLTone",
               "tempo": BPM, "timeSigNumerator": 4, "timeSigDenominator": 4, "keyRoot": 0, "scale": "minor",
               "renderSampleRate": RATE, "masterVolume": .7, "loopEnabled": True,
               "loopStart": BAR * 8, "loopEnd": BAR * 16, "tracks": tracks,
               "notebook": {"html": "<h2>Night Bloom</h2><p>108 BPM · C minor. Intro, build, instrumental hook, break and final hook.</p>"}}
    manifest.write_text(json.dumps(project, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Created {manifest}: {len(tracks)} tracks, {sum(len(t['clips']) for t in tracks)} clips, 32 bars.")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/site-redesign/Night Bloom")
    create_project(parser.parse_args().output.resolve())
