#!/usr/bin/env python3
"""Record a four-bar loop from VLTone's real UI and headless audio engine.

Requires a build containing DAW_SHOT_FRAMES support. No desktop or microphone
is captured. Generate the project with create-website-demo.py first.
"""
import argparse
import os
from pathlib import Path
import subprocess
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=ROOT / 'build/bin/VLTONE')
    parser.add_argument('--project', type=Path, default=ROOT / 'artifacts/site-redesign/Night Bloom/Night Bloom.vlt')
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    project = args.project.resolve(strict=True)
    output = ROOT / 'web/public/videos'
    output.mkdir(exist_ok=True)
    for locale in ('ru', 'en'):
        capture = ROOT / 'artifacts/website-video' / locale
        frames = capture / 'frames'
        frames.mkdir(parents=True, exist_ok=True)
        for previous in frames.glob('frame-*.png'):
            previous.unlink()
        prefs = capture / 'prefs'
        settings = prefs / 'VLT Studio/VLT Studio Pro.ini'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text('[browser]\nvisible=false\n\n[ai]\nvisible=false\n\n[contextPanel]\nfollowSelection=false\n')
        env = {k:v for k,v in os.environ.items() if not k.startswith('DAW_SHOT_')}
        env.update(QT_QPA_PLATFORM='offscreen:configfile='+str(ROOT/'scripts/screenshot-screen.json'),
                   QT_SCALE_FACTOR='1', VLT_GPU_WORKSPACE='0', DAW_PREF_DIR=str(prefs),
                   DAW_SHOT_SIZE='1440x900', DAW_SHOT_MIXER='off', DAW_SHOT_SELECT='Warm Keys',
                   DAW_SHOT_PLAYHEAD=str(60/108*32), DAW_SHOT_ROLLING='1',
                   DAW_SHOT_CYCLE=f'{60/108*32},{60/108*48}', DAW_SHOT_DELAY='1400',
                   DAW_SHOT_TRANSPORT_STYLE='plain', DAW_SHOT_DEMO_LOCALE=locale,
                   DAW_SHOT_FRAMES=str(frames), DAW_SHOT_FPS='27', DAW_SHOT_FRAME_COUNT='240')
        with (capture/'capture.log').open('w') as log:
            subprocess.run([str(executable),'--screenshot',str(capture/'end.png'),'--theme','dark',
                            '--language',locale,str(project)],cwd=ROOT/'build/bin',env=env,stdout=log,stderr=log,
                           check=True,timeout=600)
        if len(list(frames.glob('frame-*.png'))) != 240:
            raise RuntimeError('Incomplete recording; rebuild VLTONE with DAW_SHOT_FRAMES support.')
        with Image.open(frames/'frame-00000.png') as first:
            if not any(color == (174, 212, 119) and count > 50
                       for count, color in first.convert('RGB').getcolors(first.width * first.height) or []):
                raise RuntimeError('Recording uses the old theme; rebuild with the current Dark lime palette.')
        target = output/f'studio-playback-{locale}.mp4'
        staged = target.with_suffix('.new.mp4')
        subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-framerate','27',
                        '-i',str(frames/'frame-%05d.png'),'-frames:v','240','-an','-c:v','libx264',
                        '-preset','slow','-crf','23','-pix_fmt','yuv420p','-movflags','+faststart',
                        str(staged)],check=True)
        staged.replace(target)
        print(f'Recorded {locale}: 240 frames, four bars at 108 BPM.',flush=True)

if __name__ == '__main__':
    main()
