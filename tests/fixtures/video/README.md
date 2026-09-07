`background.webm` is a synthetic, silent three-second VP8 test pattern. It is
used by the local HTTP fixtures in `web_video_background_test`; tests require
neither external websites nor FFmpeg at runtime.

Generated with:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=96x54:rate=12:duration=3' -an -c:v libvpx -b:v 40k background.webm
```
