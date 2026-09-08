#!/usr/bin/env python3
"""Evaluate a frozen calibration on held-out data, without choosing thresholds."""
import argparse,hashlib,json
from pathlib import Path
from evaluate import load,known,correct,summarize,compare,wilson

def probability(entry,evidence):
    p=0
    for x,y in entry.get('knots',[]):
        if evidence<x:break
        p=y
    return p

def main():
    p=argparse.ArgumentParser();p.add_argument('--calibration',type=Path,required=True);p.add_argument('--hybrid',type=Path,required=True);p.add_argument('--dsp',type=Path,required=True);p.add_argument('--baseline',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
    config=json.loads(a.calibration.read_text());hybrid=load(a.hybrid);dsp=load(a.dsp);base=load(a.baseline)
    if any(r['split']!='test' for r in hybrid+dsp+base):raise ValueError('release validation requires held-out test rows only')
    if {r['path'] for r in hybrid}!={r['path'] for r in dsp}:raise ValueError('fallback must evaluate the same test corpus')
    fit_files=config.get('sources',{})
    for f,expected in fit_files.items():
        if hashlib.sha256(Path(f).read_bytes()).hexdigest()!=expected:raise ValueError('calibration input changed: '+f)
    fit_groups={r['group'] for f in fit_files for r in load(f)}
    if fit_groups&{r['group'] for r in hybrid}:raise ValueError('source groups leaked between fit/calibration and test')
    old=summarize(base);new=summarize(hybrid)
    improved=all(new[t]['primaryAccuracy']>old[t]['primaryAccuracy'] for t in ('tempo','key'))
    checks={}
    for name,entry in config['backends'].items():
        task,backend=name.split(':');unique={r['path']:r for r in hybrid+dsp if r[task+'Backend']==backend and (known(r,task) or (task=='key' and r.get('keyAbsent')))};rows=list(unique.values())
        threshold=.98 if task=='tempo' else .95
        confident=[r for r in rows if r[task+'Status']==2 and probability(entry,r[task+'Evidence'])>=threshold
            and not r.get(task+'Variable',False) and (task!='tempo' or r['tempoStability']>=.85)]
        count=len(confident);right=sum(correct(r,task) for r in confident)
        precision=right/count if count else None
        # Coverage is reported, never silently replaced by zero-denominator 100%.
        passed=improved and entry['calibrationCount']>=40 and count>=20 and precision is not None and precision>=threshold
        entry['validated']=passed
        checks[name]={'passed':passed,'confidentCount':count,'confidentCorrect':right,
            'confidentPrecision':precision,'coverage':count/len(rows) if rows else 0,'interval95':wilson(right,count),
            'target':threshold,'minimumTestCount':20}
    config['releaseValidated']=all(checks.get(k,{}).get('passed',False) for k in ('tempo:beat-this+grid','key:skey+hpcp'))
    config['validation']=checks
    a.output.write_text(json.dumps(config,indent=2)+'\n')
    a.report.write_text(json.dumps({'primaryImprovedBoth':improved,'baseline':old,'new':new,'equalCoverage':compare(base,hybrid),
        'confidenceGate':checks,'releasePassed':config['releaseValidated']},indent=2)+'\n')
if __name__=='__main__':main()
