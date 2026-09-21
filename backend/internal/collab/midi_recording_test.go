package collab

import (
	"encoding/json"
	"github.com/google/uuid"
	"os"
	"testing"
)

func TestMidiV4PayloadAndAtomicMetadata(t *testing.T) {
	id := func() string { return uuid.NewString() }
	track, clip, run, content := id(), id(), id(), id()
	note := map[string]any{"id": id(), "pitch": 60, "startBeats": 0.0, "lengthBeats": 1.0, "velocity": 111, "releaseVelocity": 65, "channel": 15, "startOrder": 1, "endOrder": 9, "pan": 0, "muted": false, "color": 0}
	point := map[string]any{"id": id(), "beats": 0, "value": 8193.0 / 16383, "shape": "hold", "curve": 0, "eventOrder": 2}
	lane := map[string]any{"id": id(), "name": "Bend", "cc": -2, "channel": 15, "key": 0, "parameterId": "", "slotId": "", "defaultValue": 8192.0 / 16383, "points": []any{point}}
	contents := map[string]any{"notes": []any{note}, "lanes": []any{lane}, "takes": []any{}, "comp": []any{}, "expanded": false}
	part := map[string]any{"recordingId": run, "contentId": content, "index": 0, "count": 1, "content": contents}
	encode := func(v any) json.RawMessage {
		b, err := json.Marshal(v)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	payload := encode(part)
	if err := validateCommandPayloadShapeForSchema("recording.prepareMidi", payload, true, 4); err != nil {
		t.Fatal(err)
	}
	if err := validateCommandPayloadShapeForSchema("recording.prepareMidi", payload, true, 3); err == nil {
		t.Fatal("v3 accepted MIDI preparation")
	}
	if err := validateAssembledMidiIdentities([]json.RawMessage{encode(contents)}); err != nil {
		t.Fatal(err)
	}
	if err := validateAssembledMidiIdentities([]json.RawMessage{encode(contents), encode(contents)}); err == nil {
		t.Fatal("duplicate notes across parts accepted")
	}
	for key, value := range map[string]any{"channel": 16, "releaseVelocity": 128, "startOrder": -1} {
		previous := note[key]
		note[key] = value
		if err := validateCommandPayloadShapeForSchema("recording.prepareMidi", encode(part), true, 4); err == nil {
			t.Fatalf("invalid %s accepted", key)
		}
		note[key] = previous
	}
	for _, cc := range []int{-5, -4, -3, -2, 0, 64, 127} {
		lane["cc"] = cc
		if err := validateCommandPayloadShapeForSchema("recording.prepareMidi", encode(part), true, 4); err != nil {
			t.Fatalf("CC target %d: %v", cc, err)
		}
	}
	lane["cc"] = -1
	lane["parameterId"] = "cutoff"
	if err := validateCommandPayloadShapeForSchema("recording.prepareMidi", encode(part), true, 4); err != nil {
		t.Fatal(err)
	}
	apply := map[string]any{"trackId": track, "clipId": clip, "recordingId": run, "contentId": content, "count": 1}
	commit := map[string]any{"leases": []any{}, "commands": []any{
		map[string]any{"kind": "clip.add", "payload": map[string]any{"trackId": track, "clipId": clip, "clipKind": "midi", "name": "Recorded MIDI", "startSeconds": 0, "durationSeconds": 2, "color": 0, "afterId": ""}, "preconditions": []any{}},
		map[string]any{"kind": "recording.applyMidi", "payload": apply, "preconditions": []any{}},
	}}
	if err := validateCommandPayloadShapeForSchema("recording.commit", encode(commit), true, 4); err != nil {
		t.Fatal(err)
	}
	if !containsMidiHistory("recording.commit", encode(commit), track, clip) || containsMidiHistory("recording.commit", encode(commit), track, id()) {
		t.Fatal("MIDI history proof mismatch")
	}
	take := map[string]any{"id": id(), "name": "Take", "offsetSeconds": 0, "lengthSeconds": 2, "clipOffsetSeconds": 0, "gain": 1, "muted": false, "channels": 0, "color": 0, "notes": []any{note}, "lanes": []any{lane}}
	takeAdd := encode(map[string]any{"trackId": track, "clipId": clip, "take": take, "afterId": ""})
	if err := validateCommandPayloadShapeForSchema("take.add", takeAdd, true, 4); err != nil {
		t.Fatal(err)
	}
	if refs, err := commandAssetRequirements("take.add", takeAdd, true); err != nil || len(refs) != 0 {
		t.Fatalf("MIDI take requires audio: %v", err)
	}
}

func TestSharedCppMidiWireFixture(t *testing.T) {
	content, err := os.ReadFile("../../../tests/fixtures/midi_content_v4.json")
	if err != nil {
		t.Fatal(err)
	}
	if err := validateMidiContentPayload(content); err != nil {
		t.Fatal(err)
	}
	if err := validateAssembledMidiIdentities([]json.RawMessage{content}); err != nil {
		t.Fatal(err)
	}
}
