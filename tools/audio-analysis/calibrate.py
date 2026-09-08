#!/usr/bin/env python3
"""Fit only on fit/calibration partitions; held-out test is never a fitting input.
Writes probabilities plus a release gate. Unverified calibrations cannot auto-apply.
"""
import argparse,hashlib,json
from pathlib import Path
from evaluate import load,known,correct,wilson

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--predictions',nargs='+',type=Path,required=True);p.add_argument('--models',type=Path,default=Path('models/audio-analysis'));p.add_argument('--output',type=Path,required=True);p.add_argument('--fit-fusion',action='store_true');a=p.parse_args()
    rows=[r for f in a.predictions for r in load(f)]
    if any(r['split'] not in ('fit','calibration') for r in rows):raise ValueError('test rows may not enter calibration')
    result={'algorithmVersion':2,'modelSha256':json.loads((a.models/'manifest.json').read_text())['sha256'],
        'sources':{str(f):digest(f) for f in a.predictions},'backends':{},'keyModelWeight':.65,
        'releaseValidated':False}
    fit=[r for r in rows if r['split']=='fit' and r.get('neuralScores') and r.get('profileScores') and known(r,'key')]
    if a.fit_fusion and fit:
        candidates=[]
        for step in range(21):
            w=step/20;right=0
            for r in fit:
                scores=[w*n+(1-w)*h for n,h in zip(r['neuralScores'],r['profileScores'])]
                k=max(range(24),key=scores.__getitem__)
                right+=k%12==r['expectedRoot'] and ('major' if k<12 else 'natural_minor')==r['expectedScale']
            candidates.append((right,-abs(w-.65),w))
        result['keyModelWeight']=max(candidates)[2];result['fusionFitExamples']=len(fit)
    for task in ('tempo','key'):
        calibration=[r for r in rows if r['split']=='calibration' and (known(r,task) or (task=='key' and r.get('keyAbsent'))) and not r.get('error') and r[task+'Status']>0]
        for backend in sorted({r[task+'Backend'] for r in calibration}):
            unique={r['path']:r for r in calibration if r[task+'Backend']==backend}
            observations=sorted((r[task+'Evidence'],int(correct(r,task))) for r in unique.values())
            # Pool adjacent violators; minimum-sized bins limit extreme claims
            # based on a handful of examples. Point estimates retain sample counts.
            blocks=[]
            for start in range(0,len(observations),20):
                batch=observations[start:start+20];block=[batch[0][0],sum(y for _,y in batch),len(batch)]
                blocks.append(block)
                while len(blocks)>1 and blocks[-2][1]/blocks[-2][2]>blocks[-1][1]/blocks[-1][2]:
                    last=blocks.pop();blocks[-1][1]+=last[1];blocks[-1][2]+=last[2]
            if len(blocks)>1 and blocks[-1][2]<20:
                last=blocks.pop();blocks[-1][1]+=last[1];blocks[-1][2]+=last[2]
                while len(blocks)>1 and blocks[-2][1]/blocks[-2][2]>blocks[-1][1]/blocks[-1][2]:
                    last=blocks.pop();blocks[-1][1]+=last[1];blocks[-1][2]+=last[2]
            knots=[[0,0]]+[[x,k/n] for x,k,n in blocks]
            result['backends'][task+':'+backend]={'validated':False,'knots':knots,
                'bins':[{'minimumEvidence':x,'correct':k,'count':n,'interval95':wilson(k,n)} for x,k,n in blocks],
                'calibrationCount':len(observations)}
    a.output.write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
