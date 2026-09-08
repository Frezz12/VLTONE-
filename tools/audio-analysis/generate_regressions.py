#!/usr/bin/env python3
"""Deterministic stress cases; synthetic coverage, never release calibration data."""
import argparse, math, random, struct, wave
from pathlib import Path

RATE = 22050

def drum(signal, at, amplitude=1.0, hat=False):
    start = round(at * RATE)
    rng = random.Random(start + 29)
    for i in range(min(1400, len(signal) - start)):
        if start + i < 0: continue
        decay = math.exp(-i / (70 if hat else 240))
        signal[start + i] += amplitude * decay * (rng.uniform(-1, 1) if hat else
            .7 * math.sin(2 * math.pi * 65 * i / RATE) + .3 * rng.uniform(-1, 1))

def rhythm(bpm, seconds=8, kind='steady'):
    signal = [0.0] * round(seconds * RATE)
    beat = 60 / bpm
    for bar in range(math.ceil(seconds / (beat * 4))):
        for b in range(4):
            at = (bar * 4 + b) * beat
            if at >= seconds: continue
            if kind not in ('syncopated', 'trap'):
                drum(signal, at, .8 if b == 0 else .6)
            elif b in (1, 3):
                drum(signal, at, .7, True)
        if kind in ('syncopated', 'trap'):
            for b in (0, 1.5, 2.75):
                if (bar * 4 + b) * beat < seconds:
                    drum(signal, (bar * 4 + b) * beat, .8)
        subdivision = 8 if kind == 'trap' else 4 if kind == 'dense_hats' else 2
        for h in range(subdivision * 4):
            b = h / subdivision
            if kind == 'swing' and h % 2: b += 1 / 6
            at = (bar * 4 + b) * beat
            if at < seconds: drum(signal, at, .08 if kind == 'dense_hats' else .15, True)
    return signal

def harmony(root, cents=0, seconds=8):
    signal = [0.0] * round(seconds * RATE)
    for i in range(len(signal)):
        section = min(3, int(i / RATE / (seconds / 4)))
        local = i / RATE % (seconds / 4)
        envelope = min(1, local * 30, (seconds / 4 - local) * 30)
        for interval in (0, 4, 7):
            hz = 220 * 2 ** ((root + (0, 5, 7, 0)[section] + interval - 9 + cents / 100) / 12)
            phase = 2 * math.pi * hz * i / RATE
            signal[i] += envelope * (.17 * math.sin(phase) + .04 * math.sin(2 * phase))
    return signal

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=Path('.cache/audio-analysis/regressions'))
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    cases = []
    for kind, bpm in [('swing',120), ('syncopated',110), ('dense_hats',140), ('trap',70)]:
        cases.append((kind, rhythm(bpm, kind=kind), 1, bpm, -1, '', 1))
    cases += [('short_loop', rhythm(128, 2), 1, 128, -1, '', 1),
        ('arbitrary_crop', rhythm(128, 8)[round(.173*RATE):round(4.347*RATE)], 1, 128, -1, '', 1),
        ('detuned_35_cents', harmony(0, 35), 1, 0, 0, 'major', 0),
        ('key_change', harmony(0, seconds=20)+harmony(6, seconds=20), 1, 0, -1, '', 0),
        ('tempo_change', rhythm(120, 20)+rhythm(150, 20), 1, 0, -1, '', 1),
        ('silence', [0.0]*RATE*4, 1, 0, -1, '', 1)]
    mono = rhythm(128)
    cases.append(('antiphase', [x for v in mono for x in (v, -v)], 2, 128, -1, '', 1))
    bass = [sum(.15/h*math.sin(2*math.pi*55*h*i/RATE) for h in range(1,9)) for i in range(RATE*4)]
    cases.append(('one_bass_note', bass, 1, 0, -1, '', 1))
    rows = ['path\tbpm\troot\tscale\tgroup\tsplit\tlicense\tkeyAbsent']
    for name, samples, channels, bpm, root, scale, absent in cases:
        path = args.output / (name+'.wav')
        with wave.open(str(path), 'wb') as output:
            output.setnchannels(channels); output.setsampwidth(2); output.setframerate(RATE)
            output.writeframes(struct.pack('<'+'h'*len(samples), *(round(max(-1,min(1,x))*32767) for x in samples)))
        rows.append(f'{path.resolve()}\t{bpm}\t{root}\t{scale}\t{name}\tregression\tsynthetic\t{absent}')
    (args.output/'manifest.tsv').write_text('\n'.join(rows)+'\n')

if __name__ == '__main__': main()
