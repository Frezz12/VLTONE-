# Architecture and measurement choices

## Data flow

```text
Audio producer                 Single consumer                  Optional UI
float stereo samples  -->  newest complete stereo window  -->  FFT + calibrated power
ring buffer + mailbox          read()                              |
                                                                    v
                           SpectrumFrame: frequencies, instantaneous dBFS,
                           attack/release levels, held peaks, strongest peak
                                                                    |
                                                                    v
                           visual envelope --> two-stage motion --> weighting
                                                                    |
                                                                    v
                           bounded cubic paths --> ordered transparent layers
```

No application, audio-device or plugin-framework objects appear in the core API. The host decides when to capture and consume, how many analyzers to run, how to route audio and how to render it. The examples are ordinary consumers of this API.

## FFT and calibration

A periodic Hann window reduces leakage. A precomputed radix-2 complex FFT packs the left channel in the real component and the right channel in the imaginary component, then separates both spectra. Their squared amplitudes are averaged. This avoids cancellation when left and right have opposite polarity.

The actual Hann sum supplies coherent-gain calibration. Interior bins use the usual factor of two for a one-sided amplitude spectrum; DC and Nyquist do not. A full-scale bin-centred sine in both channels reads 0 dBFS. A single active stereo channel reads about -3.01 dBFS because of power averaging.

Frequency points are logarithmic. Each display cell retains the largest enclosed FFT-bin power as well as the interpolated centre value. A narrow upper-frequency peak is therefore not discarded merely because the display has fewer points than the FFT. This produces an amplitude envelope, not a band-energy or PSD measurement. Noise height depends on FFT size and display projection.

The strongest FFT peak gets a quadratic estimate in log magnitude. This improves the readout for isolated off-bin tones but does not separate unresolved tones or remove window bias. Nyquist bounds, the configured floor and nonfinite sample handling are explicit.

FFT size and hop are independent. The default 8192-sample window at 48 kHz spans 170.67 ms; bins are 5.859 Hz apart. A shorter hop publishes more often but does not shorten the window or improve its frequency resolution. The default hop is 512; the demo uses 256 for its live presentation.

## Audio ownership

The producer owns history and a private back window; the consumer owns a front window; an atomic mailbox owns the third window. Exchanges transfer ownership, preventing either thread from accessing a vector being written by the other. A dirty bit marks a newly published window. There is no retry loop, lock, waiting or allocation in capture.

An O(N) chronological copy occurs once per hop after the first full window. The consumer executes the FFT only when it reads a new window. It can fall behind without blocking audio, at the cost of skipping intermediate measurements. Reset generations prevent old-stream history from being analyzed after a reset.

The API is single-producer/single-consumer. This is not a general multi-reader queue; prepare/destruction require stopped callers. The concurrency stress test supplements this ownership design but does not constitute exhaustive race verification on every platform.

## Three different kinds of smoothing

1. **Measurement ballistics.** Attack/release operates in dB using captured sample positions for elapsed time. These are exponential time constants: one constant traverses about 63% of a step. Peak hold has a duration followed by a linear dB/second fall. These change `levelsDb` and `peaksDb`, not `instantaneousDb`.
2. **Spatial presentation.** A log-frequency upper envelope expands narrow peaks with parabolic shoulders; a small Gaussian pass rounds the joins. It has no peak-prominence threshold that could suddenly restore a sharp spike. For a sufficiently sampled isolated peak the reduction is about one third of a dB. The width is a display choice; it can broaden and merge nearby peaks. `smoothDisplaySpectrum()` is portable and does not mutate a frame.
3. **Presentation motion.** Two cascaded one-pole stages are advanced with their exact elapsed-time solution. Their step response is critically damped, and a new target does not instantly change the visible velocity. The `transitionMs` setting is approximately the time to traverse 95% of a step. Cursor and zoom interactions remain direct. The UI timer stops when settled or hidden.

The rounded target is computed before presentation motion, so frame-to-frame peak selection cannot bypass the motion filter. Cubic rendering gives every knot one shared tangent, limited for monotonicity. Limiting tangents independently for each segment would create different slopes at their shared point; that is deliberately avoided.

## Display weighting

A conventional tilt adds `tiltDbPerOctave * log2(frequency / 1000)` dB to the picture. The separate high-frequency shelf uses a smoothstep over the log-frequency interval from `highFrequencyStartHz` to `highFrequencyEndHz`. It is exactly zero below the start and constant above the end. With defaults, +12 dB is reached at 16 kHz, and 500–2000 Hz receives no extra boost.

Both settings affect only drawing. The graph says **Display dB** when weighted, while numeric readings remain unweighted dBFS. There is no automatic gain normalization, hidden midrange bell or manipulation of the measured FFT arrays. Weighting improves visibility, but cannot reveal frequency content absent from the source.

## Before/after layers

Each trace has raw data, a rounded target and a current presentation. The host supplies independent pre/post analyzer frames; all traces use the same display mapping. They are drawn back-to-front by `zOrder`. A difference fill closes the foreground path against the reversed reference path. It highlights both boosts and cuts while leaving the original trace visible.

The demonstration uses an actual peaking biquad to generate its post-EQ samples. Its parameters are smoothed on the sample clock. Coefficient equations follow the [W3C Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/); this small example filter is not part of the analyzer library. Host processors with latency must align their analysis taps if exact temporal comparison is required.

The visual references were commercial EQ analyzers, including [FabFilter's analyzer documentation](https://www.fabfilter.com/help/pro-q/using/analyzer). Spectre is an independent implementation; it does not use their source, graphic assets or proprietary algorithms.
