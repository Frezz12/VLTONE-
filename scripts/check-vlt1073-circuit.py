"""Independent complex nodal AC check of the REDUCED network.

This is not a SPICE transcription of EH10023: missing transformer and coil
data prohibit certifying the complete original circuit from these scans.
Run after measure-vlt1073.py. Normalized RC/RLC branch impedances are stamped
into a matrix and solved by complex Gaussian elimination, independently of
the C++ trapezoidal state recurrence. Full-path coloration cancels against
the flat small-signal measurement.
"""
import csv
import json
import math
import pathlib

root = pathlib.Path('artifacts/vlt1073/measurements')
def solve(a, b):
    n = len(b)
    for col in range(n):
        pivot = max(range(col, n), key=lambda row: abs(a[row][col]))
        a[col], a[pivot], b[col], b[pivot] = a[pivot], a[col], b[pivot], b[col]
        d = a[col][col]
        a[col] = [v / d for v in a[col]]
        b[col] /= d
        for row in range(n):
            if row == col:
                continue
            q = a[row][col]
            a[row] = [v - q*w for v, w in zip(a[row], a[col])]
            b[row] -= q*b[col]
    return b

def response(frequency, gains, frequencies, digital=False):
    # Each source-to-RLC branch has a resistor, inductor and capacitor in
    # series. Its resistor voltage is the presence contribution. LF uses
    # the capacitor voltage of an RC branch; HF the resistor voltage.
    gains = list(gains)
    a = [10**(abs(g)/20) for g in gains]
    poles = [frequencies[0]/math.sqrt(a[0]), frequencies[1], 12000*math.sqrt(a[2])]
    rate = 4*48000
    omega = 2*rate*math.tan(math.pi*frequency/rate) if digital else 2*math.pi*frequency
    if digital:
        poles = [rate/math.pi*math.tan(math.pi*fc/rate) for fc in poles]
    s = complex(0, omega)
    # Unknowns: output voltage and LF/MID/HF branch currents for input/output.
    matrix = [[0j]*7 for _ in range(7)]
    rhs = [0j]*7
    matrix[0][0] = 1
    rhs[0] = 1
    for branch in range(3):
        w = 2*math.pi*poles[branch]
        if branch == 0:
            z = 1 + w/s
            observation = w/s
        elif branch == 1:
            z = .8 + s/w + w/s
            observation = .8
        else:
            z = 1 + w/s
            observation = 1
        for side in (0, 1):
            at = 1+branch+side*3
            matrix[at][at] = z
            if side:
                matrix[at][0] = -1
                matrix[0][at] = (a[branch]-1)*observation if gains[branch] < 0 else 0
            else:
                rhs[at] = 1
                matrix[0][at] = -(a[branch]-1)*observation if gains[branch] > 0 else 0
    return solve(matrix, rhs)[0]

def read(name):
    return {float(r['frequency_hz']): float(r['gain_db']) for r in csv.DictReader((root/f'vlt-{name}.csv').open())}
flat = read('flat')
results = []
for name, gains, frequencies in [
    ('hf-plus16', (0,0,16), (35,360)), ('hf-minus16', (0,0,-16), (35,360)),
    ('mid360-plus18', (0,18,0), (35,360)), ('mid7200-minus18', (0,-18,0), (35,7200)),
    ('lf35-plus16', (16,0,0), (35,360)), ('lf220-minus16', (-16,0,0), (220,360)),
    ('combined', (9,-9,9), (35,360))]:
    measured = read(name)
    errors = {}
    for digital in (False, True):
        errors[digital] = max(abs(measured[f]-flat[f]-20*math.log10(abs(response(f,gains,frequencies,digital)))) for f in measured)
    results.append({'case':name, 'analog_reduced_network_max_db':errors[False], 'trapezoidal_nodal_max_db':errors[True]})
(root/'nodal-check.json').write_text(json.dumps({'scope':'reduced network only; original EH10023 not certified','results':results},indent=2))
print(json.dumps(results,indent=2))
if max(r['trapezoidal_nodal_max_db'] for r in results) > .025:
    raise SystemExit('Reduced-network numerical check failed')
