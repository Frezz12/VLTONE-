# Diagnostic snapshots

The desktop measures process/system CPU and DSP once a second. `TelemetryClient`
flushes a separate immutable event at startup, after each five-minute window and
on normal exit (the latter can cover a partial window). CPU/DSP values are
averages of the measurements; DSP peak is the maximum of those measurements,
not a callback-level maximum. Track/audio metadata represents the time of flush.
The snapshot records elapsed window time and the number of measurements.

The version 1 optional `snapshot` contains:

- Every track's ID, name, kind, parent/output routing, volume/pan, mute/solo,
  recording/monitor/input configuration and freeze status.
- Instruments, ordered track/master/sampler effect slots, plugin format, vendor,
  version when known, bypass, mix, channel mode and sidechain source. Clip effect
  chains and offline-processing chains are listed under their owning track.
  These describe configured slots, not proof that a third-party instance is healthy.
- Sends, tempo, time signature, transport position, loop region and master controls.
- Current audio input/output names and driver API, channels, running state,
  sample rate/buffer frames, cumulative underflow/overflow counters and audio
  worker counters. Buffer duration is not measured end-to-end latency.
- Session hardware remains attached to the session and appears in report details.

No media, note data, plugin state blobs, parameters, credentials, local paths,
project author or AI instructions are collected. Metadata names are bounded and
path-like strings are redacted in the desktop and again on the server.

The desktop bounds metadata to 2,048 tracks, 256 plugins per chain and 256 sends
or clip-effect entries per track. Clip effects have a 128 KiB per-track budget;
the track/master metadata has a 1 MiB budget, leaving room for audio data and
aggregate plugins within the API's 2 MiB request limit. Exceeding a snapshot
limit sets `truncated`; the admin explicitly displays a partial-report notice.
Top-level counts still describe the entire project. The existing 250 MiB local
outbox continues to queue events while offline and retry them by event ID.

The server validates a nested metadata allowlist and stores the snapshot in the
existing `telemetry_events.payload` JSONB, with no additional schema migration.
It keeps lightweight metrics in `telemetry_samples`.

`GET /v1/admin/users/{userId}/telemetry?limit=20` returns newest-first summary
rows, recent sessions and `next_cursor`. Pass the opaque cursor as `before` to
retrieve older rows; timestamp + sample UUID ordering handles tied timestamps.
`GET /v1/admin/users/{userId}/telemetry/{eventId}` returns the sanitized full
payload, associated session hardware and event/device/time identifiers. Both
routes require admin authentication and scope data to the requested user.

The user profile polls summary rows every 30 seconds while active. Reports load
only when expanded; their state and track search survive incoming rows. Old
samples without a snapshot show their original aggregate metrics and plugins.

Deploy the backend and admin before releasing the updated desktop. Earlier
backend versions reject unknown snapshot fields. Previously uploaded reports
cannot acquire track metadata retroactively.
