"""Run local black-box measurements; does not copy any Arturia assets/code.

Usage: python scripts/measure-vlt1073.py [--probe build-windows/bin/plugin_probe.exe]
The installed reference is optional: --arturia PATH. CSVs include actual gain;
Arturia's compensated Input Gain is never equated with VLT's hardware gain.
"""
import argparse
import csv
import json
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--probe', default='build-windows/bin/plugin_probe.exe')
p.add_argument('--arturia', default=r'C:\Program Files\Common Files\VST3\Arturia\Pre 1973.vst3')
p.add_argument('--out', default='artifacts/vlt1073/measurements')
args = p.parse_args()
out = pathlib.Path(args.out)
out.mkdir(parents=True, exist_ok=True)
cases = [
    ('flat', {}, {}),
    ('hf-plus16', {'highGain': 16}, {'7': 16, '18': 16}),
    ('hf-minus16', {'highGain': -16}, {'7': -16, '18': -16}),
    ('mid360-plus18', {'midGain': 18}, {'9': 18, '20': 18}),
    ('mid7200-minus18', {'midFrequency': 6, 'midGain': -18}, {'8': 1, '19': 1, '9': -18, '20': -18}),
    ('lf35-plus16', {'lowGain': 16}, {'11': 16, '22': 16}),
    ('lf220-minus16', {'lowFrequency': 4, 'lowGain': -16}, {'10': 1, '21': 1, '11': -16, '22': -16}),
    ('hpf300', {'highPass': 4}, {'12': 1, '23': 1}),
    ('combined', {'highGain': 9, 'midGain': -9, 'lowGain': 9}, {'7': 9, '18': 9, '9': -9, '20': -9, '11': 9, '22': 9}),
    ('eq-off', {'eqEnabled': 0, 'highGain': 16}, {'3': 0, '15': 0, '7': 16, '18': 16}),
]
manifest = []
def run(name, kind, edits, level=-60, frequency=0, tone2=0):
    target = ['internal', 'daw.1073'] if kind == 'vlt' else ['vst3', args.arturia]
    if kind == 'arturia' and not pathlib.Path(args.arturia).exists():
        return
    command = [args.probe, *target, '--measure', str(out / f'{kind}-{name}.csv'),
               '--sample-rate', '48000', '--block-size', '257', '--amplitude-db', str(level)]
    if kind == 'arturia':
        edits = {'0': 0, '1': 0, '2': 1, '5': 0, '14': 0, '6': -80, '17': -80, **edits}
    for key, value in edits.items():
        command += ['--set', key, str(value)]
    if frequency:
        command += ['--frequency', str(frequency)]
    if tone2:
        command += ['--tone2', str(tone2)]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
    (out / f'{kind}-{name}.txt').write_text(result.stdout + result.stderr, encoding='utf-8')
    manifest.append({'name': name, 'kind': kind, 'command': command, 'exit': result.returncode})
    print(kind, name, result.returncode, flush=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)

for name, vlt, arturia in cases:
    run(name, 'vlt', vlt)
    run(name, 'arturia', arturia)
for level in (-36, -18, -6, 0):
    for gain in (0, 10, 20):
        run(f'input{gain}-signal{level}', 'vlt', {'lineGain': gain}, level, 1000)
    for gain in (-80, -50, -20):
        run(f'input{gain}-signal{level}', 'arturia', {'6': gain, '17': gain}, level, 1000)
for name, edits in [('level0', {'lineGain': 20}), ('level-minus12', {'lineGain': 20, 'level': -12}), ('output-minus12', {'lineGain': 20, 'output': -12})]:
    run(name, 'vlt', edits, -6, 1000)
run('imd', 'vlt', {'lineGain': 20}, -18, 19000, 20000)
run('imd', 'arturia', {'6': -20, '17': -20}, -18, 19000, 20000)
run('alias', 'vlt', {'lineGain': 20}, -6, 17000)
run('alias', 'arturia', {'6': -20, '17': -20}, -6, 17000)
(out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
comparisons = []
def read(kind, name):
    return {float(row['frequency_hz']): row for row in csv.DictReader((out / f'{kind}-{name}.csv').open())}
if pathlib.Path(args.arturia).exists():
    flat_v, flat_a = read('vlt', 'flat'), read('arturia', 'flat')
    for name, _, _ in cases:
        vlt, arturia = read('vlt', name), read('arturia', name)
        deviations = {f: (float(vlt[f]['gain_db']) - float(flat_v[f]['gain_db'])) -
                          (float(arturia[f]['gain_db']) - float(flat_a[f]['gain_db'])) for f in vlt}
        worst = max(deviations, key=lambda f: abs(deviations[f]))
        comparisons.append({'case': name, 'maximum_eq_difference_db': abs(deviations[worst]), 'at_hz': worst})
    (out / 'comparison.json').write_text(json.dumps(comparisons, indent=2), encoding='utf-8')
    print(json.dumps(comparisons, indent=2))
