#!/usr/bin/env python3
"""Strict primary-answer metrics, coverage and equal-coverage comparisons."""
import argparse, json, math, statistics
from pathlib import Path

def load(path):
    rows = [json.loads(line) for line in Path(path).read_text().splitlines() if line]
    if len({r['path'] for r in rows}) != len(rows): raise ValueError('duplicate source rows')
    return rows

def rounded(x): return math.floor(x + .5) if math.isfinite(x) and x > 0 else 0

def known(r, task): return r['expectedBpm'] > 0 if task == 'tempo' else r['expectedRoot'] >= 0

def correct(r, task):
    if r.get('error'): return False
    if task == 'tempo': return r['tempoStatus'] > 0 and rounded(r['bpm']) == rounded(r['expectedBpm'])
    if r.get('keyAbsent'): return r['keyStatus'] == 0
    return r['keyStatus'] > 0 and r['root'] == r['expectedRoot'] and r['scale'] == r['expectedScale']

def wilson(success, total):
    if not total: return [0, 1]
    p = success / total; z = 1.96; d = 1 + z*z/total
    c = (p + z*z/(2*total))/d
    h = z*math.sqrt(p*(1-p)/total + z*z/(4*total*total))/d
    return [max(0,c-h), min(1,c+h)]

def summarize(rows):
    result = {'files':len(rows), 'failed':sum(bool(r.get('error')) for r in rows),
              'medianSeconds':statistics.median(r['seconds'] for r in rows) if rows else 0}
    for task in ('tempo','key'):
        labeled = [r for r in rows if known(r,task)]
        high = [r for r in labeled if r[task+'High']]
        right = sum(correct(r,task) for r in labeled); high_right = sum(correct(r,task) for r in high)
        result[task] = {'labeled':len(labeled), 'correctPrimary':right,
            'primaryAccuracy':right/len(labeled) if labeled else None,
            'confidentCount':len(high), 'confidentCoverage':len(high)/len(labeled) if labeled else 0,
            'confidentAccuracy':high_right/len(high) if high else None,
            'confident95Interval':wilson(high_right,len(high)),
            'availableCoverage':sum(r[task+'Status']>0 for r in labeled)/len(labeled) if labeled else 0}
        if task == 'tempo':
            detected = [r for r in labeled if r['tempoStatus']>0 and not r.get('error')]
            result[task].update({'meanAbsoluteBpmError':statistics.mean(abs(r['bpm']-r['expectedBpm']) for r in detected) if detected else None,
              'halfTempoErrors':sum(not correct(r,task) and abs(r['bpm']*2/r['expectedBpm']-1)<.01 for r in detected),
              'doubleTempoErrors':sum(not correct(r,task) and abs(r['bpm']*.5/r['expectedBpm']-1)<.01 for r in detected),
              'correctAlternativeOnly':sum(not correct(r,task) and any(rounded(v)==rounded(r['expectedBpm']) for v in r['tempoAlternatives']) for r in labeled)})
        else:
            absent=[r for r in rows if r.get('keyAbsent')]
            result[task].update({'notTonalExamples':len(absent),'falseKeyPrimary':sum(r['keyStatus']>0 for r in absent),'confidentFalseKey':sum(r['keyHigh'] for r in absent)})
            result[task]['correctAlternativeOnly']=sum(not correct(r,task) and r['alternateRoot']==r['expectedRoot'] and r['alternateScale']==r['expectedScale'] for r in labeled)
    return result

def compare(a,b):
    if {r['path'] for r in a}!={r['path'] for r in b}: raise ValueError('baseline and new analyzer must evaluate identical sources')
    out={}
    for task in ('tempo','key'):
        group={}
        for name,rows in [('baseline',a),('new',b)]:
            ranked=sorted((r for r in rows if known(r,task)),key=lambda r:(r.get(task+'Evidence',r[task+'Confidence']),r['path']),reverse=True)
            group[name]={}
            for coverage in (.1,.25,.5,.75,1):
                n=max(1,math.ceil(len(ranked)*coverage));chosen=ranked[:n]
                group[name][str(coverage)]={'count':len(chosen),'precision':sum(correct(r,task) for r in chosen)/len(chosen) if chosen else None}
        out[task]=group
    return out

def main():
    p=argparse.ArgumentParser();p.add_argument('predictions',type=Path);p.add_argument('--baseline',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
    rows=load(a.predictions);report={'new':summarize(rows)}
    if a.baseline:
        baseline=load(a.baseline);report['baseline']=summarize(baseline);report['equalCoverage']=compare(baseline,rows)
    text=json.dumps(report,indent=2)+'\n'
    if a.output:a.output.write_text(text)
    else:print(text)
if __name__=='__main__':main()
