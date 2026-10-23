# VLTONE integration

Vendored from https://github.com/Frezz12/VLT-Analyze-Spectre at
`3c4ed2f42e346cd8e2ca94330f0871082bc3c380` (version 0.1.0, MIT).
Upstream sources are unchanged. Retain `LICENSE` when updating this snapshot.

`daw_pluginhost` links `VltSpectre::Core`; the optional widget and examples are
not built. `EqualizerInstance` captures actual input, output and sidechain audio
into three analyzers. The existing editor and rack consume the same telemetry
on the UI thread, where Spectre performs the FFT. The audio thread only captures
samples and resets streams; it never reads frames or performs analysis FFTs.

The host uses an 8192-sample FFT, a 512-sample hop and 768 logarithmic points
from 20 Hz to 20 kHz. Its existing tilt is applied to display telemetry after
Spectre's calibrated stereo-power measurement. Freeze keeps capture live.
Preparation and destruction require both audio and telemetry calls to stop.

To update, replace the upstream snapshot, keep this integration note, update
the pinned commit above, and run the upstream DSP checks plus `equalizer_test`
and `plugin_readout_test` in VLTONE.
