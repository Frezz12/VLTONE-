# Signalsmith time stretching

Vendored without local modifications, under the included MIT licenses:

- Signalsmith Stretch 1.3.2, commit `57b93f4e9206a089a45387eaa39bdc9f310d3308`
  from https://github.com/Signalsmith-Audio/signalsmith-stretch
- Signalsmith Linear 0.3.1, commit `5668673560146a9cfe38c25315071e3fd68c8317`
  from https://github.com/Signalsmith-Audio/linear

Only headers and licenses are required. No download happens during configure
or build. The engine's private adapter is `engine/DSP/TimeStretch.cpp`.
It uses the portable FFT backend on every platform and keeps compiler
optimisation enabled for this DSP translation unit in Clang/GCC Debug builds.
Use Release for realtime work with MSVC.

Large ratios are decomposed into multiple stages of at most 2x each by the
adapter. This avoids the upstream extreme-ratio phase randomisation and
amplitude beating without changing its phase predictor.
