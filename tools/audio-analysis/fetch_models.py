#!/usr/bin/env python3
"""Developer-only fetch of pinned upstream checkpoints; never used by the app."""
import argparse,hashlib,tarfile,urllib.request
from pathlib import Path
ASSETS={
 'final0.ckpt':('https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp/final0.ckpt','8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331'),
 'skey-source.tgz':('https://github.com/deezer/skey/archive/918b83d273568d5041569bb8068843d19a335726.tar.gz','1e512812aa7586e88b0bbdad626e3527cf29951bfec628d3ed740318cf7ebddd')}
def main():
 p=argparse.ArgumentParser();p.add_argument('--cache',type=Path,default=Path('.cache/audio-analysis'));a=p.parse_args();a.cache.mkdir(parents=True,exist_ok=True)
 for name,(url,expected) in ASSETS.items():
  out=a.cache/name
  if not out.exists() or hashlib.sha256(out.read_bytes()).hexdigest()!=expected:
   with urllib.request.urlopen(url,timeout=120) as response: data=response.read(100*1024*1024)
   if hashlib.sha256(data).hexdigest()!=expected:raise ValueError('checkpoint checksum mismatch: '+name)
   out.write_bytes(data)
 with tarfile.open(a.cache/'skey-source.tgz') as archive:archive.extractall(a.cache,filter='data')
if __name__=='__main__':main()
