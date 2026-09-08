#!/usr/bin/env python3
"""Official GiantSteps preview backup; audio is local evaluation data only."""
import argparse,concurrent.futures,hashlib,json,tarfile,urllib.request
from pathlib import Path

def main():
    p=argparse.ArgumentParser();p.add_argument('--cache',type=Path,default=Path('.cache/audio-analysis'));p.add_argument('--limit',type=int,default=40);a=p.parse_args()
    a.cache.mkdir(parents=True, exist_ok=True)
    archives={
        'giantsteps-key.tgz':('GiantSteps/giantsteps-key-dataset','6bcd492c825ac9b8597bc650a5f6fd18b6c43d2b'),
        'giantsteps-tempo.tgz':('GiantSteps/giantsteps-tempo-dataset','d51ab2422e76abacfaa86616a57054bc222ec9fd'),
        'beat-training-annotations.tgz':('CPJKU/beat_this_annotations','c3c47fd37d3074d9f8119f18bbf460f909609f22')}
    for name,(repo,commit) in archives.items():
        path=a.cache/name
        if not path.exists():
            with urllib.request.urlopen(f'https://github.com/{repo}/archive/{commit}.tar.gz',timeout=60) as response:
                path.write_bytes(response.read(32*1024*1024))
        with tarfile.open(path) as archive:
            if archive.pax_headers.get('comment')!=commit:raise ValueError('wrong annotation revision: '+name)
    keyzip=tarfile.open(a.cache/'giantsteps-key.tgz');tempozip=tarfile.open(a.cache/'giantsteps-tempo.tgz')
    keys={Path(n).name.removesuffix('.key'):keyzip.extractfile(n).read().decode().strip() for n in keyzip.getnames() if '/annotations/key/' in n and n.endswith('.key')}
    tempos={Path(n).name.removesuffix('.bpm'):float(tempozip.extractfile(n).read().decode()) for n in tempozip.getnames() if '/annotations_v2/tempo/' in n and n.endswith('.bpm')}
    checks={Path(n).name.removesuffix('.md5'):keyzip.extractfile(n).read().decode().split()[0] for n in keyzip.getnames() if '/md5/' in n and n.endswith('.md5')}
    training=tarfile.open(a.cache/'beat-training-annotations.tgz')
    training_ids={Path(n).stem for n in training.getnames() if n.endswith('.beats') and '/gtzan/' not in n}
    eligible=sorted(set(keys)&set(tempos),key=lambda x:hashlib.sha256(('vltone-giantsteps-v2:'+x).encode()).hexdigest())
    excluded=[x for x in eligible if x in training_ids or x.split('.')[0] in training_ids]
    selected=[x for x in eligible if x not in excluded][:a.limit]
    root={'C':0,'C#':1,'Db':1,'D':2,'D#':3,'Eb':3,'E':4,'F':5,'F#':6,'Gb':6,'G':7,'G#':8,'Ab':8,'A':9,'A#':10,'Bb':10,'B':11}
    folder=a.cache/'giantsteps';folder.mkdir(exist_ok=True)
    def fetch(name):
        path=folder/(name+'.mp3');expected=checks[name]
        if not path.exists() or hashlib.md5(path.read_bytes()).hexdigest()!=expected:
            url='https://www.cp.jku.at/datasets/giantsteps/backup/'+name+'.mp3'
            with urllib.request.urlopen(url,timeout=60) as response:content=response.read(10*1024*1024)
            if hashlib.md5(content).hexdigest()!=expected:raise ValueError('audio checksum mismatch: '+name)
            path.write_bytes(content)
        note,mode=keys[name].split()
        return f'{path.resolve()}\t{tempos[name]}\t{root[note]}\t'+('natural_minor' if mode.lower()=='minor' else 'major')+f'\t{name}\ttest\tresearch-preview'
    rows=['path\tbpm\troot\tscale\tgroup\tsplit\tlicense']
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for index,row in enumerate(pool.map(fetch,selected)):
            rows.append(row);(folder/'manifest.tsv').write_text('\n'.join(rows)+'\n');print(index+1,len(selected),flush=True)
    (folder/'provenance.json').write_text(json.dumps({'archives':{f:hashlib.sha256((a.cache/f).read_bytes()).hexdigest() for f in ['giantsteps-key.tgz','giantsteps-tempo.tgz','beat-training-annotations.tgz']},'excludedTrainingIds':excluded,'selected':selected,'knownIdOverlapCheckOnly':True},indent=2)+'\n')
if __name__=='__main__':main()
