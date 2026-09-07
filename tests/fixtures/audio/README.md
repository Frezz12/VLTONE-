`mp3-vbr-estimated-length.mp3` is a synthetic stereo 440 Hz tone with a
100 ms silent start. It contains no third-party recording. Generated with:

```sh
ffmpeg -f lavfi \
  -i 'aevalsrc=if(lt(t\,0.1)\,0\,0.4*sin(440*2*PI*t)):s=44100:d=2' \
  -ac 2 -c:a libmp3lame -q:a 4 -write_xing 0 mp3-vbr-estimated-length.mp3
```

Without a Xing length header, libsndfile 1.2.2 reports 101342 frames but
successfully decodes 89856 frames (including encoder delay/padding), with no
error. The old strict decode loops rejected this valid stream as truncated.
Keep the fixture in the test suite so running tests does not require FFmpeg.

`aac-stereo.m4a` and `aac-mono.aac` are also synthetic, generated with:

```sh
ffmpeg -f lavfi -i 'aevalsrc=0.3*sin(440*2*PI*t)|0.2*sin(660*2*PI*t):s=44100:d=1.25' -c:a aac -b:a 160k aac-stereo.m4a
ffmpeg -f lavfi -i 'sine=frequency=330:sample_rate=48000:duration=0.75' -c:a aac -b:a 96k -f adts aac-mono.aac
```
