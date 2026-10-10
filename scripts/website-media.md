# Website media

The marketing images and hero recording use the current **Dark** theme with the
lime accent (`#AED477`), in Russian and English. Creator media is maintained
separately and must not be overwritten by these scripts.

Use a freshly built `build/bin/VLTONE` alongside its normal helper executables:

```sh
python3 scripts/create-website-demo.py
python3 scripts/refresh-studio-screenshots.py --originals artifacts/website-current
python3 scripts/capture-website-demo.py --executable build/bin/VLTONE --publish
python3 scripts/capture-website-video.py
```

The showcase capture rejects an executable with the old Dark palette. Review the
PNG originals in `artifacts` before publishing. Sampler needs time to finish its
asynchronous editor initialization. Generic captures run first; the richer
Night Bloom project then replaces the arrangement, MIDI, mixer and EQ views.

The hero is a real Qt widget recording of four bars of the generated project at
108 BPM: 240 frames at 27 fps, 1440 × 900, H.264 MP4 with fast start and no audio
track. `DAW_SHOT_FRAMES`, `DAW_SHOT_FPS` and `DAW_SHOT_FRAME_COUNT` extend the
existing screenshot command. Between frames it advances the real audio engine;
it does not capture a desktop, microphone or personal project. Run with
`VLT_GPU_WORKSPACE=0`, as the Python script does.

The page loops the recording and pauses it when the hero leaves the viewport,
the tab is hidden, motion is paused, or reduced motion is enabled. A current
screenshot remains the poster when JavaScript or autoplay is unavailable.
After refreshing media, bump `web/lib/screenshot-version.ts` to avoid stale
browser caches. The general manual is text only and has no image dependency.
