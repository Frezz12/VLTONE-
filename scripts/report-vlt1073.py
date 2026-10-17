"""Plot recorded measurements; never infer hardware/UAD equivalence from them."""
import csv
import pathlib

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

root = pathlib.Path('artifacts/vlt1073')
measurements = root / 'measurements'

def response(kind, case):
    with (measurements / f'{kind}-{case}.csv').open() as file:
        rows = list(csv.DictReader(file))
    return np.array([float(r['frequency_hz']) for r in rows]), np.array([float(r['gain_db']) for r in rows])

fig, axes = plt.subplots(2, 3, figsize=(13, 7), sharex=True, constrained_layout=True)
cases = ['hf-plus16', 'hf-minus16', 'mid360-plus18', 'mid7200-minus18', 'lf35-plus16', 'hpf300']
for ax, case in zip(axes.flat, cases):
    for kind, name, color in [('vlt', 'VLT 1073 reduced model', '#227b9b'), ('arturia', 'Pre 1973 Type 1', '#a94b43')]:
        x, y = response(kind, case)
        _, flat = response(kind, 'flat')
        ax.semilogx(x, y-flat, color=color, label=name, linewidth=1.7)
    ax.set_title(case, fontsize=10)
    ax.set_xlim(20, 20000)
    ax.grid(True, which='both', alpha=.16)
    ax.set_ylabel('EQ / own flat response, dB')
    ax.set_xlabel('Frequency, Hz')
axes[0, 0].legend(fontsize=8)
fig.suptitle('Measured small-signal EQ response — 48 kHz, −60 dBFS peak\n'
             'Arturia comparison only; this is not an original-circuit or hardware certification', fontsize=12)
fig.savefig(root / 'frequency-comparison.png', dpi=160)
plt.close(fig)

# A diagnostic contour plot of the actual Qt capture against the supplied
# panel. This is a comparison figure, not an edited UI asset.
from PIL import Image
reference = pathlib.Path(r'C:\Users\FrezzY\AppData\Local\Temp\codex-clipboard-0735be89-d642-4ffc-8047-778c7fe8c31c.png')
actual = root / 'dpi-100' / 'vlt1073-reference.png'
if reference.exists() and actual.exists():
    old = np.asarray(Image.open(reference).convert('RGB'))[44:638, 177:429]
    new = np.asarray(Image.open(actual).convert('RGB').resize((252, 594), Image.Resampling.LANCZOS))
    fig, axes = plt.subplots(1, 3, figsize=(8, 6.8), constrained_layout=True)
    axes[0].imshow(old)
    axes[0].set_title('Supplied reference')
    axes[1].imshow(new)
    axes[1].set_title('Actual Qt panel')
    axes[2].imshow(np.full((594, 252, 3), .98))
    for pixels, color in [(old, '#ba635a'), (new, '#387f9b')]:
        grey = pixels.astype(float).mean(axis=2)
        gy, gx = np.gradient(grey)
        edge = np.hypot(gx, gy)
        axes[2].contour(edge, levels=[35], colors=[color], linewidths=.35)
    axes[2].set_title('Contours: reference / Qt')
    for ax in axes:
        ax.set_xlim(0, 252)
        ax.set_ylim(594, 0)
        ax.set_aspect('equal')
        ax.axis('off')
    fig.savefig(root / 'panel-contours.png', dpi=180)
    plt.close(fig)
print('Plots saved in', root)
