package collab

import (
	"encoding/json"
	"github.com/google/uuid"
	"testing"
)

func TestV6RenderStateContracts(t *testing.T) {
	asset := map[string]any{"assetId": uuid.NewString(), "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "byteSize": 128, "kind": "audio", "originalName": "render.wav"}
	source := map[string]any{"asset": asset, "durationSeconds": 8, "offsetSeconds": 0, "fadeInSeconds": 0, "fadeOutSeconds": 0, "fadeInCurve": 0, "fadeOutCurve": 0, "fadeInMode": "gain", "fadeOutMode": "gain", "gain": 1, "pan": 0, "channels": 2, "takes": []any{}, "comp": []any{}, "compCrossfadeMs": 5, "sampleEdit": extensionSampleEdit(), "analysis": json.RawMessage(`{"version":1,"offsetSeconds":0,"durationSeconds":8,"tempo":{"status":2,"bpm":127.86,"confidence":0.99,"stability":1,"alternatives":[64],"variable":false},"key":{"status":1,"root":0,"scale":"major","confidence":0.8,"alternateRoot":9,"alternateScale":"natural_minor","tuningCents":0}}`), "warp": map[string]any{"enabled": false, "preservePitch": true, "mode": 4, "baselineDurationSeconds": 0, "sensitivity": 50, "markers": []any{}}}
	version := uuid.NewString()
	clip := uuid.NewString()
	body := map[string]any{"trackId": uuid.NewString(), "clipId": clip, "source": source, "history": []any{map[string]any{"id": version, "parentId": "", "label": "Original", "source": source}}, "versionId": version, "injection": map[string]any{"stage": "none", "anchorChannelId": ""}}
	encoded, _ := json.Marshal(body)
	fields, _, err := deriveCommandMetadataForSchema("clip.setRenderState", encoded, true, 6)
	if err != nil {
		t.Fatal(err)
	}
	if !equalStrings(fields, []string{"clip:" + clip + ":descendants", "clip:" + clip + ":renderState", "project:renderGeneration"}) {
		t.Fatalf("wrong render fields: %v", fields)
	}
	requirements, err := commandAssetRequirements("clip.setRenderState", encoded, true)
	if err != nil || len(requirements) != 2 {
		t.Fatalf("history asset not enforced: %v %v", requirements, err)
	}
	if validateCommandPayloadShapeForSchema("clip.setRenderState", encoded, true, 5) == nil {
		t.Fatal("legacy accepts render state")
	}
	source["filePath"] = "C:/local.wav"
	encoded, _ = json.Marshal(body)
	if validateCommandPayloadShapeForSchema("clip.setRenderState", encoded, true, 6) == nil {
		t.Fatal("local render path accepted")
	}
	delete(source, "filePath")
	body["versionId"] = uuid.NewString()
	encoded, _ = json.Marshal(body)
	if validateCommandPayloadShapeForSchema("clip.setRenderState", encoded, true, 6) == nil {
		t.Fatal("missing history version accepted")
	}
	freeze, _ := json.Marshal(map[string]any{"trackId": uuid.NewString(), "asset": nil, "durationSeconds": 0, "sampleRate": 0})
	if err := validateCommandPayloadShapeForSchema("track.setFreeze", freeze, true, 6); err != nil {
		t.Fatal(err)
	}
}

func TestEditLeaseHierarchy(t *testing.T) {
	field := "plugin:" + uuid.NewString()
	if !editLeaseFieldsOverlap(field, field+":param:gain") || editLeaseFieldsOverlap(field, "plugin:"+uuid.NewString()+":param:gain") {
		t.Fatal("bad hierarchical lease overlap")
	}
	if _, err := normalizeEditLeaseFields([]string{"samplerFx:" + uuid.NewString(), "project:masterVolume", "send:" + uuid.NewString() + ":level"}); err != nil {
		t.Fatalf("mixer lease keys rejected: %v", err)
	}
	for _, fields := range [][]string{{"project:renderGeneration"}, {"clip:*"}, {"recording:" + uuid.NewString() + ":other"}, {field, field}, {}} {
		if _, err := normalizeEditLeaseFields(fields); err == nil {
			t.Fatalf("invalid lease accepted: %v", fields)
		}
	}
}

func TestV6TrackMuteUsesAudition(t *testing.T) {
	payload, _ := json.Marshal(map[string]any{"trackId": uuid.NewString(), "property": "muted", "value": true})
	if err := validateCommandPayloadShapeForSchema("track.setProperty", payload, true, 5); err != nil {
		t.Fatalf("historical mute no longer replays: %v", err)
	}
	if validateCommandPayloadShapeForSchema("track.setProperty", payload, true, 6) == nil {
		t.Fatal("v6 audition mute was accepted as a document edit")
	}
}

func TestV6RecordingFadeOnlyTouchesCreatedClipWithoutLease(t *testing.T) {
	track, clip := uuid.NewString(), uuid.NewString()
	fade := map[string]any{"trackId": track, "clipId": clip, "fadeInSeconds": 0.01, "fadeOutSeconds": 0.02}
	commands := []any{
		map[string]any{"kind": "clip.add", "payload": map[string]any{"trackId": track, "clipId": clip, "clipKind": "audio", "name": "Recorded", "startSeconds": 0, "durationSeconds": 2, "color": 0, "afterId": ""}, "preconditions": []any{}},
		map[string]any{"kind": "clip.setFade", "payload": fade, "preconditions": []any{}},
	}
	encode := func() []byte {
		value, _ := json.Marshal(map[string]any{"leases": []any{}, "commands": commands})
		return value
	}
	if err := validateCommandPayloadShapeForSchema("recording.commit", encode(), true, 6); err != nil {
		t.Fatal(err)
	}
	if validateCommandPayloadShapeForSchema("recording.commit", encode(), true, 5) == nil {
		t.Fatal("legacy recording accepted new fade child")
	}
	fade["clipId"] = uuid.NewString()
	if validateCommandPayloadShapeForSchema("recording.commit", encode(), true, 6) == nil {
		t.Fatal("lease-free recording changed another clip's fade")
	}
}
