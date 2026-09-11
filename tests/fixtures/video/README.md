`background.webm` is a synthetic, silent three-second VP8 test pattern. It is
used by the local HTTP fixtures in `web_video_background_test`; tests require
neither external websites nor FFmpeg at runtime.

Generated with:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=96x54:rate=12:duration=3' -an -c:v libvpx -b:v 40k background.webm
```

`background.mp4` is the same fixture transcoded to H.264/yuv420p for the native
Qt Multimedia smoke test (the macOS AVFoundation backend does not decode VP8).
The application still accepts formats supported by its installed Qt backend.

```sh
ffmpeg -i background.webm -c:v libx264 -pix_fmt yuv420p -an -movflags +faststart background.mp4
```
