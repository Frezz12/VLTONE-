#!/usr/bin/env python3
"""Fetch a reproducible, human-annotated CC0/CC-BY subset using ZIP range reads.
Audio stays in .cache, never in a shipped application or source fixture pack.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import time
import urllib.request
import zipfile

URL = 'https://zenodo.org/records/3967852/files/FSL10K.zip?download=1'
SIZE = 8841802341

class RemoteZip(io.RawIOBase):
    def __init__(self, cache):
        self.position = 0
        tail = cache / 'fsld-tail2.bin'
        self.block = tail.read_bytes() if tail.exists() else b''
        self.start = SIZE - len(self.block)
        self.cache = cache
    def seekable(self): return True
    def tell(self): return self.position
    def seek(self, offset, whence=0):
        self.position = offset if whence == 0 else (self.position if whence == 1 else SIZE) + offset
        return self.position
    def read(self, size=-1):
        if size < 0: size = SIZE - self.position
        if not size: return b''
        if not (self.start <= self.position and self.position + size <= self.start + len(self.block)):
            count = min(SIZE - self.position, max(size, 262144))
            if count > 128 * 1024 * 1024: raise ValueError('refusing unexpectedly large ZIP member')
            request = urllib.request.Request(URL, headers={'Range': f'bytes={self.position}-{self.position+count-1}'})
            for retry in range(6):
                try:
                    with urllib.request.urlopen(request, timeout=60) as response:
                        if response.status != 206: raise ValueError('server did not honor byte range')
                        if not response.headers.get('Content-Range', '').startswith(f'bytes {self.position}-'):
                            raise ValueError('server returned the wrong range')
                        self.block = response.read(count + 1)
                        if len(self.block) != count: raise ValueError('wrong range length')
                    break
                except Exception:
                    if retry == 5: raise
                    time.sleep(min(10, 2 ** retry))
            self.start = self.position
        data = self.block[self.position-self.start:self.position-self.start+size]
        self.position += len(data)
        return data

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cache', type=Path, default=Path('.cache/audio-analysis'))
    parser.add_argument('--limit', type=int, default=400)
    parser.add_argument('--inspect', action='store_true')
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    annotations_path = args.cache / 'fsld-annotations.zip'
    expected = 'e062f1f8e298730df92a879c302ce2c4f718039a3979442abc8aaca1258ac38e'
    if not annotations_path.exists():
        with urllib.request.urlopen('https://zenodo.org/records/3967852/files/annotations.zip?download=1', timeout=60) as response:
            annotations_path.write_bytes(response.read(8 * 1024 * 1024))
    if hashlib.sha256(annotations_path.read_bytes()).hexdigest() != expected:
        raise ValueError('annotation checksum mismatch')
    with zipfile.ZipFile(args.cache / 'fsld-annotations.zip') as annotations:
        rows = {}
        for name in annotations.namelist():
            if not name.endswith('.json'): continue
            item = json.loads(annotations.read(name))
            if item.get('discard') or item.get('save_for_later'): continue
            sound = Path(name).stem.removeprefix('sound-')
            rows.setdefault(sound, []).append(item)
    z = zipfile.ZipFile(RemoteZip(args.cache))
    metadata_path = args.cache / 'fsld-metadata.json'
    if not metadata_path.exists():
        name = next(x for x in z.namelist() if Path(x).name == 'metadata.json')
        metadata_path.write_bytes(z.read(name))
    metadata = json.loads(metadata_path.read_text())
    if args.inspect:
        print(z.namelist()[:10]); print(type(metadata)); print(str(metadata)[:3000]);
        print('annotations', len(rows)); return
    pitch = {'C':0,'C#':1,'Db':1,'D':2,'D#':3,'Eb':3,'E':4,'F':5,'F#':6,'Gb':6,'G':7,'G#':8,'Ab':8,'A':9,'A#':10,'Bb':10}
    candidates = []
    for sound, entries in rows.items():
        meta = metadata.get(sound, {})
        license = meta.get('license', '')
        if not ('/publicdomain/zero/' in license or '/licenses/by/' in license): continue
        # This release's metadata has no duration; bound ZIP members below.
        answers = {(str(x.get('bpm', '0')) if x.get('defined_tempo') else '0', x.get('key', 'none'), x.get('mode', 'none')) for x in entries}
        if len(answers) != 1: continue
        bpm, key, mode = next(iter(answers))
        root = pitch.get(key.capitalize(), -1)
        scale = 'natural_minor' if mode.lower() in ('minor', 'min') else 'major' if mode.lower() in ('major', 'maj') else ''
        if root >= 0 and not scale: continue
        bpm = float(bpm) if bpm else 0
        if bpm <= 0 and root < 0: continue
        group = str(meta.get('username', meta.get('user', sound)))
        candidates.append((sound, meta, bpm, root, scale, group))
    candidates.sort(key=lambda x: hashlib.sha256(('vltone-analysis-v2:' + x[0]).encode()).hexdigest())
    selected = candidates[:args.limit]
    folder = args.cache / 'fsld'
    folder.mkdir(exist_ok=True)
    names = [n for n in z.namelist() if Path(n).suffix.lower() in ('.wav', '.aif', '.aiff', '.flac', '.ogg', '.mp3')]
    manifest = ['path\tbpm\troot\tscale\tgroup\tsplit\tlicense\tkeyAbsent']
    for idx, (sound, meta, bpm, root, scale, group) in enumerate(selected):
        name = next((n for n in names if Path(n).stem == sound or Path(n).name.startswith(sound + '.')), None)
        if name is None:
            name = next((n for n in names if Path(n).name.startswith(sound + '_') or Path(n).name.startswith(sound + '-')), None)
        if name is None: raise ValueError(('missing audio', sound, meta, names[:15]))
        if z.getinfo(name).file_size > 32 * 1024 * 1024: continue
        out = folder / (sound + Path(name).suffix)
        if not out.exists(): out.write_bytes(z.read(name))
        bucket = int(hashlib.sha256(('group:' + group).encode()).hexdigest()[:8], 16) % 10
        split = 'fit' if bucket < 5 else 'calibration' if bucket < 7 else 'test'
        absent = int(all(x.get('key') == 'none' and x.get('mode') == 'none' for x in rows[sound]))
        manifest.append(f'{out.resolve()}\t{bpm}\t{root}\t{scale}\t{group}\t{split}\t{meta["license"]}\t{absent}')
        (folder / 'manifest.tsv').write_text('\n'.join(manifest) + '\n')
        print(f'{idx+1}/{len(selected)} {sound} {split}', flush=True)
    print('eligible', len(candidates), 'selected', len(selected), flush=True)

if __name__ == '__main__': main()
