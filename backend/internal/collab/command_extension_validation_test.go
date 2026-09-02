package collab

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/google/uuid"
)

func TestTakeMoveV2GoldenFixture(t *testing.T) {
	contents, err := os.ReadFile(filepath.Join("..", "..", "..", "tests", "fixtures", "collaboration_take_move_v2.json"))
	if err != nil {
		t.Fatal(err)
	}
	var fixture struct {
		Kind          string          `json:"kind"`
		Payload       json.RawMessage `json:"payload"`
		TouchedFields []string        `json:"touchedFields"`
	}
	if err := json.Unmarshal(contents, &fixture); err != nil {
		t.Fatal(err)
	}
	fields, _, err := deriveCommandMetadata(fixture.Kind, fixture.Payload, true)
	if err != nil {
		t.Fatalf("golden take.move was rejected: %v", err)
	}
	if !equalStrings(fields, fixture.TouchedFields) {
		t.Fatalf("fields = %v, want %v", fields, fixture.TouchedFields)
	}
	steps, err := deriveLifecycleSteps(fixture.Kind, fixture.Payload, true)
	if err != nil || len(steps) != 1 || len(steps[0].Requirements) != 4 || len(steps[0].Mutations) != 0 {
		t.Fatalf("take.move lifecycle = %#v, %v", steps, err)
	}

	var payload map[string]any
	if err := json.Unmarshal(fixture.Payload, &payload); err != nil {
		t.Fatal(err)
	}
	payload["afterId"] = payload["takeId"]
	selfAnchor, _ := json.Marshal(payload)
	if err := validateCommandPayloadShape("take.move", selfAnchor, true); !errors.Is(err, ErrValidation) {
		t.Fatalf("self-anchored take.move returned %v", err)
	}
	delete(payload, "afterId")
	missingAnchor, _ := json.Marshal(payload)
	if err := validateCommandPayloadShape("take.move", missingAnchor, true); !errors.Is(err, ErrValidation) {
		t.Fatalf("take.move without afterId returned %v", err)
	}
}

func TestCommandV2FocusedGoldenFixture(t *testing.T) {
	contents, err := os.ReadFile(filepath.Join("..", "..", "..", "tests", "fixtures", "collaboration_command_v2_golden.json"))
	if err != nil {
		t.Fatal(err)
	}
	var fixture struct {
		Commands []struct {
			Kind          string          `json:"kind"`
			Payload       json.RawMessage `json:"payload"`
			TouchedFields []string        `json:"touchedFields"`
		} `json:"commands"`
	}
	if err := json.Unmarshal(contents, &fixture); err != nil {
		t.Fatal(err)
	}
	if len(fixture.Commands) == 0 {
		t.Fatal("focused command golden fixture is empty")
	}
	for _, command := range fixture.Commands {
		t.Run(command.Kind, func(t *testing.T) {
			fields, _, err := deriveCommandMetadata(command.Kind, command.Payload, true)
			if err != nil {
				t.Fatalf("golden command was rejected: %v", err)
			}
			if !equalStrings(fields, command.TouchedFields) {
				t.Fatalf("fields = %v, want %v", fields, command.TouchedFields)
			}
		})
	}
}

func extensionAsset(id uuid.UUID, kind, digest string) map[string]any {
	return map[string]any{
		"assetId": id.String(), "sha256": strings.Repeat(digest, 64),
		"kind": kind, "byteSize": 512, "originalName": "asset.bin",
	}
}

func extensionLocation(chain string, trackID, clipID uuid.UUID) map[string]any {
	track, clip := "", ""
	if trackID != uuid.Nil {
		track = trackID.String()
	}
	if clipID != uuid.Nil {
		clip = clipID.String()
	}
	return map[string]any{"chain": chain, "trackId": track, "clipId": clip}
}

func extensionInsert(id uuid.UUID, uid string, bindings []any) map[string]any {
	return map[string]any{
		"id": id.String(), "name": "Built-in", "bypassed": false,
		"format": "internal", "uid": uid, "vendor": "VLTONE",
		"pluginVersion": "1.0.0", "stateSchemaVersion": 1, "mix": 1.0,
		"channelMode": "auto", "sidechainTrackId": "", "stateAsset": nil,
		"rightStateAsset": nil, "parameters": []any{}, "rightParameters": []any{},
		"assetBindings": bindings,
	}
}

func extensionSampleEdit() map[string]any {
	return map[string]any{
		"loopMode": 0, "loopStart": 0.0, "loopEnd": 1.0,
		"stretchMode": 0, "stretchTime": 1.0, "stretchPitch": 0.0,
		"formant": 0.0, "rootNote": 60, "boost": 0.0, "eqLow": 0.0,
		"eqMid": 0.0, "eqHigh": 0.0, "ringMix": 0.0, "ringFreq": 0.5,
		"cut": 1.0, "res": 0.0, "reverbType": 0, "reverb": 0.0,
		"stereoDelay": 0.0, "pogo": 0.0, "removeDc": false,
		"reversePolarity": false, "normalize": false, "fadeStereo": false,
		"reverse": false, "swapStereo": false,
	}
}

func TestEmptySamplerBindingLifecycleValidation(t *testing.T) {
	trackID, insertID, sampleID := uuid.New(), uuid.New(), uuid.New()
	location := extensionLocation("instrument", trackID, uuid.Nil)
	tests := []struct {
		kind    string
		payload map[string]any
	}{
		{"plugin.add", map[string]any{
			"location": location, "insert": extensionInsert(insertID, "daw.sampler", []any{}), "afterId": "",
		}},
		{"plugin.setAssetBinding", map[string]any{
			"location": location, "insertId": insertID.String(),
			"binding": map[string]any{"key": "sample", "asset": extensionAsset(sampleID, "audio", "e"), "required": true},
		}},
		{"plugin.removeAssetBinding", map[string]any{
			"location": location, "insertId": insertID.String(), "key": "sample",
		}},
	}
	for _, test := range tests {
		raw, err := json.Marshal(test.payload)
		if err != nil {
			t.Fatal(err)
		}
		if err := validateCommandPayloadShape(test.kind, raw, true); err != nil {
			t.Fatalf("valid empty-Sampler lifecycle %s was rejected: %v", test.kind, err)
		}
	}
}

func TestMixerPluginAndClipCommandsStayInValidationLockstep(t *testing.T) {
	trackID, destinationID, clipID := uuid.New(), uuid.New(), uuid.New()
	sourceTrackID := uuid.MustParse("00000000-0000-4000-8000-000000000001")
	sendID, insertID, operationID, assetID := uuid.New(), uuid.New(), uuid.New(), uuid.New()
	location := extensionLocation("track", trackID, uuid.Nil)
	clipLocation := extensionLocation("clip", trackID, clipID)
	tests := []struct {
		kind     string
		payload  map[string]any
		expected []string
	}{
		{"track.setParent", map[string]any{"trackId": trackID.String(), "parentId": destinationID.String()}, []string{"track:" + trackID.String() + ":parentId"}},
		{"track.setOutput", map[string]any{"trackId": trackID.String(), "outputTrackId": destinationID.String()}, []string{"track:" + trackID.String() + ":outputTrackId"}},
		{"send.add", map[string]any{"trackId": trackID.String(), "send": map[string]any{"id": sendID.String(), "destinationTrackId": destinationID.String(), "level": 0.5, "preFader": false, "enabled": true}, "afterId": ""}, []string{"send:" + sendID.String() + ":lifecycle", "send:" + sendID.String() + ":position"}},
		{"send.delete", map[string]any{"trackId": trackID.String(), "sendId": sendID.String()}, []string{"send:" + sendID.String() + ":lifecycle"}},
		{"send.restore", map[string]any{"trackId": trackID.String(), "sendId": sendID.String(), "deleteOperationId": operationID.String()}, []string{"send:" + sendID.String() + ":lifecycle", "send:" + sendID.String() + ":position"}},
		{"send.move", map[string]any{"trackId": trackID.String(), "sendId": sendID.String(), "afterId": ""}, []string{"send:" + sendID.String() + ":position"}},
		{"send.setProperty", map[string]any{"trackId": trackID.String(), "sendId": sendID.String(), "property": "destinationTrackId", "value": destinationID.String()}, []string{"send:" + sendID.String() + ":destinationTrackId"}},
		{"clip.setAsset", map[string]any{"trackId": trackID.String(), "clipId": clipID.String(), "asset": extensionAsset(assetID, "audio", "a")}, []string{"clip:" + clipID.String() + ":asset", "track:" + trackID.String() + ":clipLanding"}},
		{"clip.setSampleEdit", map[string]any{"trackId": trackID.String(), "clipId": clipID.String(), "sampleEdit": extensionSampleEdit()}, []string{"clip:" + clipID.String() + ":sampleEdit", "track:" + trackID.String() + ":clipLanding"}},
		{"clip.setProperty", map[string]any{"trackId": trackID.String(), "clipId": clipID.String(), "property": "startSeconds", "value": 1.5}, []string{"clip:" + clipID.String() + ":startSeconds", "project:tempoCascade", "track:" + trackID.String() + ":clipLanding"}},
		{"clip.move", map[string]any{"sourceTrackId": sourceTrackID.String(), "trackId": trackID.String(), "clipId": clipID.String(), "afterId": ""}, []string{"clip:" + clipID.String() + ":position", "track:" + sourceTrackID.String() + ":clipLanding", "track:" + trackID.String() + ":clipLanding"}},
		{"plugin.add", map[string]any{"location": location, "insert": extensionInsert(insertID, "daw.equalizer", []any{}), "afterId": ""}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":lifecycle", "plugin:" + insertID.String() + ":position"}},
		{"plugin.add", map[string]any{"location": extensionLocation("instrument", trackID, uuid.Nil), "insert": extensionInsert(insertID, "daw.sampler", []any{}), "afterId": ""}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":lifecycle", "plugin:" + insertID.String() + ":position"}},
		{"plugin.delete", map[string]any{"location": location, "insertId": insertID.String()}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":lifecycle"}},
		{"plugin.restore", map[string]any{"location": location, "insertId": insertID.String(), "deleteOperationId": operationID.String()}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":lifecycle", "plugin:" + insertID.String() + ":position"}},
		{"plugin.move", map[string]any{"location": location, "insertId": insertID.String(), "afterId": ""}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":position"}},
		{"plugin.setProperty", map[string]any{"location": location, "insertId": insertID.String(), "property": "mix", "value": 0.25}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":mix"}},
		{"plugin.setProperty", map[string]any{"location": clipLocation, "insertId": insertID.String(), "property": "mix", "value": 0.25}, []string{"clip:" + clipID.String() + ":descendants", "plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":mix"}},
		{"plugin.setState", map[string]any{"location": location, "insertId": insertID.String(), "pluginVersion": "1.1.0", "stateSchemaVersion": 2, "stateAsset": nil, "rightStateAsset": nil, "parameters": []any{}, "rightParameters": []any{}, "assetBindings": []any{}}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":state"}},
		{"plugin.setParameter", map[string]any{"location": location, "insertId": insertID.String(), "parameterId": "gain", "value": 0.75, "rightChannel": false}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":parameter:left:gain", "plugin:" + insertID.String() + ":state"}},
		{"plugin.removeParameter", map[string]any{"location": location, "insertId": insertID.String(), "parameterId": "gain", "rightChannel": true}, []string{"plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":parameter:right:gain", "plugin:" + insertID.String() + ":state"}},
		{"plugin.setAssetBinding", map[string]any{"location": location, "insertId": insertID.String(), "binding": map[string]any{"key": "impulse", "asset": extensionAsset(assetID, "plugin-resource", "a"), "required": false}}, []string{"plugin:" + insertID.String() + ":assetBinding:impulse", "plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":state"}},
		{"plugin.removeAssetBinding", map[string]any{"location": location, "insertId": insertID.String(), "key": "impulse"}, []string{"plugin:" + insertID.String() + ":assetBinding:impulse", "plugin:" + insertID.String() + ":generation", "plugin:" + insertID.String() + ":state"}},
	}
	for _, test := range tests {
		t.Run(test.kind, func(t *testing.T) {
			payload, err := json.Marshal(test.payload)
			if err != nil {
				t.Fatal(err)
			}
			fields, _, err := deriveCommandMetadata(test.kind, payload, true)
			if err != nil {
				t.Fatalf("valid payload was rejected: %v", err)
			}
			if !equalStrings(fields, test.expected) {
				t.Fatalf("fields = %v, want %v", fields, test.expected)
			}
		})
	}
}

func TestExtendedCommandValidationRejectsDivergentPayloads(t *testing.T) {
	trackID, clipID, insertID := uuid.New(), uuid.New(), uuid.New()
	validLocation := extensionLocation("track", trackID, uuid.Nil)
	invalid := []struct {
		kind    string
		payload map[string]any
	}{
		{"track.setOutput", map[string]any{"trackId": trackID.String(), "outputTrackId": trackID.String()}},
		{"send.setProperty", map[string]any{"trackId": trackID.String(), "sendId": uuid.NewString(), "property": "level", "value": 2.0}},
		{"clip.setSampleEdit", func() map[string]any {
			edit := extensionSampleEdit()
			edit["loopMode"] = 0.5
			return map[string]any{"trackId": trackID.String(), "clipId": clipID.String(), "sampleEdit": edit}
		}()},
		{"plugin.add", func() map[string]any {
			insert := extensionInsert(insertID, "daw.equalizer", []any{})
			insert["path"] = "/Library/Audio/Plug-Ins/leak"
			return map[string]any{"location": validLocation, "insert": insert, "afterId": ""}
		}()},
		{"plugin.add", func() map[string]any {
			insert := extensionInsert(insertID, "com.vendor.external", []any{})
			return map[string]any{"location": validLocation, "insert": insert, "afterId": ""}
		}()},
		{"plugin.add", map[string]any{"location": extensionLocation("instrument", trackID, uuid.Nil), "insert": extensionInsert(insertID, "daw.sampler", []any{
			map[string]any{"key": "sample", "asset": extensionAsset(uuid.New(), "audio", "d"), "required": false},
		}), "afterId": ""}},
		{"plugin.setState", map[string]any{
			"location": extensionLocation("instrument", trackID, uuid.Nil), "insertId": insertID.String(),
			"pluginVersion": "1.0.0", "stateSchemaVersion": 1, "stateAsset": nil,
			"rightStateAsset": nil, "parameters": []any{}, "rightParameters": []any{},
			"assetBindings": []any{map[string]any{
				"key": "sample", "asset": extensionAsset(uuid.New(), "audio", "d"), "required": false,
			}},
		}},
		{"plugin.setAssetBinding", map[string]any{
			"location": extensionLocation("instrument", trackID, uuid.Nil), "insertId": insertID.String(),
			"binding": map[string]any{
				"key": "sample", "asset": extensionAsset(uuid.New(), "audio", "d"), "required": false,
			},
		}},
		{"plugin.setParameter", map[string]any{"location": validLocation, "insertId": insertID.String(), "parameterId": strings.Repeat("x", maximumPluginParameterIDBytes+1), "value": 1.0, "rightChannel": false}},
		{"plugin.delete", map[string]any{"location": extensionLocation("clip", trackID, uuid.Nil), "insertId": insertID.String()}},
	}
	for _, test := range invalid {
		payload, err := json.Marshal(test.payload)
		if err != nil {
			t.Fatal(err)
		}
		if err := validateCommandPayloadShape(test.kind, payload, true); !errors.Is(err, ErrValidation) {
			t.Fatalf("invalid %s payload returned %v", test.kind, err)
		}
	}
}

func TestExtendedLifecycleLeaseAndAssetMetadata(t *testing.T) {
	trackID, clipID, sendID, pluginID := uuid.New(), uuid.New(), uuid.New(), uuid.New()
	deleteOperationID := uuid.New()
	location := extensionLocation("track", trackID, uuid.Nil)
	deleteThenEdit, err := json.Marshal(map[string]any{"commands": []any{
		map[string]any{"kind": "send.delete", "payload": map[string]any{"trackId": trackID.String(), "sendId": sendID.String()}, "preconditions": []any{}},
		map[string]any{"kind": "send.setProperty", "payload": map[string]any{"trackId": trackID.String(), "sendId": sendID.String(), "property": "enabled", "value": true}, "preconditions": []any{}},
	}})
	if err != nil {
		t.Fatal(err)
	}
	steps, err := deriveLifecycleSteps("batch", deleteThenEdit, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := validateLifecycleSequence(map[string]lifecycleState{}, deleteOperationID, steps); !errors.Is(err, ErrEntityDeleted) {
		t.Fatalf("send delete-wins lifecycle returned %v", err)
	}

	routingPayload, _ := json.Marshal(map[string]any{"trackId": trackID.String(), "outputTrackId": ""})
	policy, err := deriveCommandLeasePolicy("track.setOutput", routingPayload, true)
	if err != nil || len(policy.RoutedTrackIDs) != 1 || policy.RoutedTrackIDs[0] != trackID {
		t.Fatalf("routing lease policy = %#v, %v", policy, err)
	}

	stateID, sampleID, resourceID := uuid.New(), uuid.New(), uuid.New()
	sampler := extensionInsert(pluginID, "daw.sampler", []any{
		map[string]any{"key": "sample", "asset": extensionAsset(sampleID, "audio", "b"), "required": true},
		map[string]any{"key": "mapping", "asset": extensionAsset(resourceID, "plugin-resource", "c"), "required": false},
	})
	sampler["stateAsset"] = extensionAsset(stateID, "plugin-state", "a")
	pluginPayload, _ := json.Marshal(map[string]any{
		"location": extensionLocation("instrument", trackID, uuid.Nil),
		"insert":   sampler, "afterId": "",
	})
	requirements, err := commandAssetRequirements("plugin.add", pluginPayload, true)
	if err != nil || len(requirements) != 3 {
		t.Fatalf("plugin asset requirements = %#v, %v", requirements, err)
	}
	if !storageAssetKindMatches("plugin-state", "plugin_state") ||
		!storageAssetKindMatches("plugin-resource", "sample") ||
		!storageAssetKindMatches("audio", "audio") ||
		storageAssetKindMatches("plugin-state", "audio") {
		t.Fatal("wire/storage asset-kind mapping is inconsistent")
	}

	pluginDelete, _ := json.Marshal(map[string]any{"location": location, "insertId": pluginID.String()})
	field, effect, err := commandLifecycleMutation("plugin.delete", pluginDelete)
	if err != nil || field != "plugin:"+pluginID.String()+":lifecycle" || effect != lifecycleDeleted {
		t.Fatalf("plugin lifecycle mutation = %q, %q, %v", field, effect, err)
	}
	_ = clipID
}
