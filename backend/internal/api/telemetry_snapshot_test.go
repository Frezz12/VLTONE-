package api

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestTelemetrySnapshotValidation(t *testing.T) {
	legacy := telemetrySamplePayload{PlaybackState: "stopped"}
	if err := validateSample(&legacy); err != nil {
		t.Fatalf("legacy sample: %v", err)
	}
	snapshot := telemetrySnapshot{SchemaVersion: 1, Tempo: 120, Tracks: []telemetryTrack{{ID: "track-1", Name: "Бас", Kind: "Instrument", Instrument: &telemetrySlot{Name: "Synth", Vendor: "Vendor", Version: "1.2", Mix: 1}, Inserts: []telemetrySlot{{Name: "/Users/test/private", Mix: .5, Bypassed: true}}}}}
	snapshot.Audio.Output = telemetryAudioDevice{Name: "USB Interface", HostAPI: "Core Audio", OutputChannels: 2}
	sample := telemetrySamplePayload{PlaybackState: "playing", Snapshot: &snapshot}
	if err := validateSample(&sample); err != nil {
		t.Fatal(err)
	}
	if snapshot.Tracks[0].Name != "Бас" || snapshot.Tracks[0].Instrument.Version != "1.2" || snapshot.Tracks[0].Inserts[0].Name != "<path-redacted>" || !snapshot.Tracks[0].Inserts[0].Bypassed {
		t.Fatalf("metadata not preserved/sanitized: %+v", snapshot)
	}
	encoded, _ := json.Marshal(sample)
	if strings.Contains(string(encoded), "/Users") {
		t.Fatal("path leaked")
	}
	for _, raw := range []string{
		`{"playback_state":"playing","snapshot":{"schema_version":1,"tempo":120,"tracks":[{"name":"Bass","file_path":"secret"}]}}`,
		`{"playback_state":"playing","snapshot":{"schema_version":1,"tempo":120,"tracks":[{"instrument":{"state":"opaque"}}]}}`,
	} {
		var value telemetrySamplePayload
		if strictUnmarshal([]byte(raw), &value) == nil {
			t.Fatal("unlisted nested field accepted")
		}
	}
	for _, change := range []func(*telemetrySnapshot){
		func(s *telemetrySnapshot) { s.SchemaVersion = 99 },
		func(s *telemetrySnapshot) { s.Tracks = make([]telemetryTrack, 2049) },
		func(s *telemetrySnapshot) { s.MasterInserts = make([]telemetrySlot, 257) },
		func(s *telemetrySnapshot) { s.Audio.InputOverflow = -1 },
		func(s *telemetrySnapshot) { s.Tracks = []telemetryTrack{{Instrument: &telemetrySlot{Mix: 2}}} },
	} {
		invalid := telemetrySnapshot{SchemaVersion: 1, Tempo: 120}
		change(&invalid)
		if validateSnapshot(&invalid) == nil {
			t.Fatal("invalid snapshot accepted")
		}
	}
}
