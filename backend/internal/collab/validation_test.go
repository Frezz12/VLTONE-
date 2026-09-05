package collab

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/model"
)

func TestRoleMatrix(t *testing.T) {
	tests := []struct {
		role       string
		permission Permission
		allowed    bool
	}{
		{"owner", PermissionManageProject, true},
		{"owner", PermissionManageMembers, true},
		{"editor", PermissionView, true},
		{"editor", PermissionEdit, true},
		{"editor", PermissionHostSession, true},
		{"editor", PermissionManageMembers, false},
		{"viewer", PermissionView, true},
		{"viewer", PermissionJoinSession, true},
		{"viewer", PermissionEdit, false},
		{"viewer", PermissionAcquireLease, false},
		{"unknown", PermissionView, false},
	}
	for _, test := range tests {
		if got := RoleAllows(test.role, test.permission); got != test.allowed {
			t.Fatalf("RoleAllows(%q, %q) = %v, want %v", test.role, test.permission, got, test.allowed)
		}
	}
}

func TestReaperParameterBounds(t *testing.T) {
	if err := validateReaperParameters(45*time.Second, 256); err != nil {
		t.Fatalf("safe reaper settings were rejected: %v", err)
	}
	for _, test := range []struct {
		staleAfter time.Duration
		limit      int
	}{
		{29 * time.Second, 256},
		{11 * time.Minute, 256},
		{45 * time.Second, 0},
		{45 * time.Second, MaximumReaperBatch + 1},
	} {
		if err := validateReaperParameters(test.staleAfter, test.limit); !errors.Is(err, ErrValidation) {
			t.Fatalf("unsafe reaper settings (%s, %d) returned %v",
				test.staleAfter, test.limit, err)
		}
	}
}

func TestCreateInviteRequiresExactDesktopActorSession(t *testing.T) {
	store := &Store{}
	base := CreateInviteInput{
		ProjectID: uuid.New(), ActorUserID: uuid.New(), ActorDeviceID: uuid.New(),
		ActorSessionID: uuid.New(), Role: model.ProjectRoleEditor,
	}
	for _, clear := range []func(*CreateInviteInput){
		func(input *CreateInviteInput) { input.ProjectID = uuid.Nil },
		func(input *CreateInviteInput) { input.ActorUserID = uuid.Nil },
		func(input *CreateInviteInput) { input.ActorDeviceID = uuid.Nil },
		func(input *CreateInviteInput) { input.ActorSessionID = uuid.Nil },
	} {
		input := base
		clear(&input)
		if _, err := store.CreateInvite(context.Background(), input); !errors.Is(err, ErrValidation) {
			t.Fatalf("invite without exact actor provenance returned %v", err)
		}
	}
}

func TestNormalizeOperationCanonicalizesRetryHash(t *testing.T) {
	trackID := uuid.New()
	entityField := "track:" + trackID.String() + ":entity"
	positionField := "track:" + trackID.String() + ":position"
	base := AppendOperationInput{
		ProjectID: uuid.New(), ActorUserID: uuid.New(), ActorDeviceID: uuid.New(),
		ActorSessionID: uuid.New(),
		OpID:           uuid.New(), Kind: "track.move", SchemaVersion: CollaborationCommandSchemaVersion, BaseSeq: 7,
		Payload: json.RawMessage(`{"trackId":"` + trackID.String() + `","afterId":""}`),
		Preconditions: []FieldPrecondition{
			{Kind: "fieldWriterIs", FieldKey: entityField, OperationID: uuid.New()},
			{Kind: "fieldWriterIs", FieldKey: positionField, OperationID: uuid.New()},
		},
		TouchedFields: []string{positionField},
	}
	first, err := normalizeOperation(base)
	if err != nil {
		t.Fatal(err)
	}
	reordered := base
	reordered.Payload = json.RawMessage(`{"afterId":"", "trackId":"` + trackID.String() + `"}`)
	reordered.Preconditions[0], reordered.Preconditions[1] = reordered.Preconditions[1], reordered.Preconditions[0]
	second, err := normalizeOperation(reordered)
	if err != nil {
		t.Fatal(err)
	}
	if first.RequestHash != second.RequestHash {
		t.Fatalf("semantically identical retry hashes differ: %s != %s", first.RequestHash, second.RequestHash)
	}
}

func TestNormalizeOperationAcceptsLockedCommandShape(t *testing.T) {
	input := AppendOperationInput{
		ProjectID: uuid.New(), ActorUserID: uuid.New(), ActorDeviceID: uuid.New(),
		ActorSessionID: uuid.New(),
		OpID:           uuid.New(), Kind: "project.setScalar", SchemaVersion: CollaborationCommandSchemaVersion,
		Payload: json.RawMessage(`{"field":"tempo","value":132}`),
		Preconditions: []FieldPrecondition{
			{Kind: "fieldWriterIs", FieldKey: "project:tempo", OperationID: uuid.New()},
		},
		TouchedFields: []string{"project:tempo", "project:tempoCascade"},
	}
	if _, err := normalizeOperation(input); err != nil {
		t.Fatalf("locked command shape was rejected: %v", err)
	}
}

func TestParseOperationUUID(t *testing.T) {
	valid := uuid.New()
	parsed, err := ParseOperationUUID(valid.String(), false)
	if err != nil || parsed == nil || *parsed != valid {
		t.Fatalf("valid operation UUID parsed as %v, %v", parsed, err)
	}
	if parsed, err := ParseOperationUUID("", true); err != nil || parsed != nil {
		t.Fatalf("empty optional transaction UUID parsed as %v, %v", parsed, err)
	}
	for _, malformed := range []string{
		"op-tempo", "00000000000000000000000000000001", " " + valid.String(),
		"00000000-0000-0000-0000-000000000000",
	} {
		if _, err := ParseOperationUUID(malformed, false); !errors.Is(err, ErrValidation) {
			t.Fatalf("malformed operation UUID %q returned %v", malformed, err)
		}
	}
}

func TestSyntheticLifecycleFieldKeyIsSafe(t *testing.T) {
	field := "tracks/" + uuid.NewString() + "/$lifecycle"
	if err := validateFieldKey(field); err != nil {
		t.Fatalf("synthetic lifecycle field key was rejected: %v", err)
	}
}

func TestNormalizeOperationRejectsUnsafeShapes(t *testing.T) {
	valid := AppendOperationInput{
		ProjectID: uuid.New(), ActorUserID: uuid.New(), ActorDeviceID: uuid.New(),
		ActorSessionID: uuid.New(), OpID: uuid.New(),
		Kind: "project.setScalar", SchemaVersion: CollaborationCommandSchemaVersion,
		Payload: json.RawMessage(`{"field":"tempo","value":120}`), TouchedFields: []string{"project:tempo", "project:tempoCascade"},
	}
	tests := []AppendOperationInput{
		func() AppendOperationInput { value := valid; value.Kind = "Track Rename"; return value }(),
		func() AppendOperationInput { value := valid; value.Payload = json.RawMessage(`[]`); return value }(),
		func() AppendOperationInput { value := valid; value.BaseSeq = -1; return value }(),
		func() AppendOperationInput { value := valid; value.TouchedFields = nil; return value }(),
		func() AppendOperationInput {
			value := valid
			value.Preconditions = []FieldPrecondition{{Kind: "fieldWriterIs", FieldKey: "bad\nkey", OperationID: uuid.New()}}
			return value
		}(),
	}
	for _, input := range tests {
		if _, err := normalizeOperation(input); !errors.Is(err, ErrValidation) {
			t.Fatalf("invalid operation was accepted or returned the wrong error: %v", err)
		}
	}
}

func TestNormalizeOperationDerivesBatchFieldsAndGuards(t *testing.T) {
	trackID := uuid.New()
	expectedWriter := uuid.New()
	payload, err := json.Marshal(map[string]any{
		"commands": []any{
			map[string]any{
				"kind":    "track.delete",
				"payload": map[string]any{"trackId": trackID.String()},
				"preconditions": []any{map[string]any{
					"kind": "fieldWriterIs", "fieldKey": "track:" + trackID.String() + ":lifecycle",
					"operationId": expectedWriter.String(),
				}},
			},
			map[string]any{
				"kind":          "project.setScalar",
				"payload":       map[string]any{"field": "tempo", "value": 128},
				"preconditions": []any{},
			},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	input := AppendOperationInput{
		ProjectID: uuid.New(), ActorUserID: uuid.New(), ActorDeviceID: uuid.New(),
		ActorSessionID: uuid.New(), OpID: uuid.New(),
		Kind: "batch", SchemaVersion: CollaborationCommandSchemaVersion, Payload: payload,
		TouchedFields: []string{
			"track:" + trackID.String() + ":lifecycle",
			"project:tempo",
			"project:tempoCascade",
		},
	}
	normalized, err := normalizeOperation(input)
	if err != nil {
		t.Fatalf("valid batch was rejected: %v", err)
	}
	if len(normalized.Preconditions) != 1 || normalized.Preconditions[0].OperationID != expectedWriter {
		t.Fatalf("batch child guard was not normalized: %#v", normalized.Preconditions)
	}
	if !normalized.LeasePolicy.BlocksProjectTiming ||
		len(normalized.LeasePolicy.DeletedTrackIDs) != 1 ||
		normalized.LeasePolicy.DeletedTrackIDs[0] != trackID {
		t.Fatalf("batch lease policy was not derived: %#v", normalized.LeasePolicy)
	}

	input.TouchedFields = input.TouchedFields[:1]
	if _, err := normalizeOperation(input); !errors.Is(err, ErrValidation) {
		t.Fatalf("batch with omitted touched field returned %v", err)
	}
}

func TestDeriveRecordingCommitLeasePolicy(t *testing.T) {
	trackID, clipID, takeID, assetID, leaseID := uuid.New(), uuid.New(), uuid.New(), uuid.New(), uuid.New()
	takePayload := map[string]any{
		"trackId": trackID.String(), "clipId": clipID.String(), "afterId": "",
		"take": map[string]any{
			"id": takeID.String(), "name": "Take", "offsetSeconds": 0.0,
			"lengthSeconds": 1.0, "clipOffsetSeconds": 0.0, "gain": 1.0,
			"muted": false, "channels": 2, "color": 0,
			"asset": map[string]any{
				"assetId": assetID.String(), "sha256": strings.Repeat("a", 64),
				"kind": "audio", "byteSize": 512, "originalName": "take.wav",
			},
		},
	}
	payload, err := json.Marshal(takePayload)
	if err != nil {
		t.Fatal(err)
	}
	policy, err := deriveCommandLeasePolicy("take.add", payload, true)
	if err != nil {
		t.Fatalf("take lease policy was rejected: %v", err)
	}
	if len(policy.RecordingLeases) != 0 {
		t.Fatalf("bare take.add must not consume a recording lease: %#v", policy)
	}
	commitPayload, err := json.Marshal(map[string]any{
		"leases": []any{map[string]any{
			"trackId": trackID.String(), "leaseId": leaseID.String(),
		}},
		"commands": []any{map[string]any{
			"kind": "take.add", "payload": takePayload, "preconditions": []any{},
		}},
	})
	if err != nil {
		t.Fatal(err)
	}
	policy, err = deriveCommandLeasePolicy("recording.commit", commitPayload, true)
	if err != nil {
		t.Fatalf("recording commit lease policy was rejected: %v", err)
	}
	if len(policy.RecordingLeases) != 1 ||
		policy.RecordingLeases[0] != (recordingLeaseReference{TrackID: trackID, LeaseID: leaseID}) {
		t.Fatalf("recording commit lease policy = %#v", policy)
	}
	fields, _, err := deriveCommandMetadata("recording.commit", commitPayload, true)
	if err != nil || !equalStrings(fields, []string{
		"clip:" + clipID.String() + ":descendants",
		"take:" + takeID.String() + ":clipOffsetSeconds",
		"take:" + takeID.String() + ":color",
		"take:" + takeID.String() + ":gain",
		"take:" + takeID.String() + ":lengthSeconds",
		"take:" + takeID.String() + ":lifecycle",
		"take:" + takeID.String() + ":muted",
		"take:" + takeID.String() + ":name",
		"take:" + takeID.String() + ":offsetSeconds",
		"take:" + takeID.String() + ":position",
		"track:" + trackID.String() + ":clipLanding",
	}) {
		t.Fatalf("recording commit metadata = %v, %v", fields, err)
	}
	requirements, err := commandAssetRequirements("recording.commit", commitPayload, true)
	if err != nil || len(requirements) != 1 || requirements[0].AssetID != assetID {
		t.Fatalf("recording commit asset requirements = %#v, %v", requirements, err)
	}
}

func TestRecordingCommitPayloadIsStrictAndTrackComplete(t *testing.T) {
	trackA := uuid.MustParse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")
	trackB := uuid.MustParse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")
	leaseA, leaseB := uuid.New(), uuid.New()
	clipID, takeID, segmentID, assetID := uuid.New(), uuid.New(), uuid.New(), uuid.New()
	take := func(trackID uuid.UUID) map[string]any {
		return map[string]any{
			"kind": "take.add",
			"payload": map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(), "afterId": "",
				"take": map[string]any{
					"id": takeID.String(), "name": "Take", "offsetSeconds": 0.0,
					"lengthSeconds": 1.0, "clipOffsetSeconds": 0.0, "gain": 1.0,
					"muted": false, "channels": 2, "color": 0,
					"asset": map[string]any{
						"assetId": assetID.String(), "sha256": strings.Repeat("c", 64),
						"kind": "audio", "byteSize": 512, "originalName": "take.wav",
					},
				},
			},
			"preconditions": []any{},
		}
	}
	valid := map[string]any{
		"leases":   []any{map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()}},
		"commands": []any{take(trackA)},
	}
	validate := func(body map[string]any) error {
		raw, err := json.Marshal(body)
		if err != nil {
			t.Fatal(err)
		}
		return validateCommandPayloadShape("recording.commit", raw, true)
	}
	if err := validate(valid); err != nil {
		t.Fatalf("valid recording commit was rejected: %v", err)
	}
	clipLanding := map[string]any{
		"leases": []any{map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()}},
		"commands": []any{
			map[string]any{
				"kind": "clip.add",
				"payload": map[string]any{
					"trackId": trackA.String(), "clipId": clipID.String(), "clipKind": "audio",
					"name": "Recording", "startSeconds": 0.0, "durationSeconds": 1.0, "color": 0,
				},
				"preconditions": []any{},
			},
			map[string]any{
				"kind": "clip.setProperty",
				"payload": map[string]any{
					"trackId": trackA.String(), "clipId": clipID.String(),
					"property": "gain", "value": 0.75,
				},
				"preconditions": []any{},
			},
			map[string]any{
				"kind": "clip.setAsset",
				"payload": map[string]any{
					"trackId": trackA.String(), "clipId": clipID.String(),
					"asset": map[string]any{
						"assetId": assetID.String(), "sha256": strings.Repeat("c", 64),
						"kind": "audio", "byteSize": 512, "originalName": "clip.wav",
					},
				},
				"preconditions": []any{},
			},
			take(trackA),
			map[string]any{
				"kind": "compSegment.upsert",
				"payload": map[string]any{
					"trackId": trackA.String(), "clipId": clipID.String(),
					"segment": map[string]any{
						"id": segmentID.String(), "takeId": takeID.String(),
						"startSeconds": 0.0, "endSeconds": 1.0,
					},
					"afterId": "",
				},
				"preconditions": []any{},
			},
		},
	}
	if err := validate(clipLanding); err != nil {
		t.Fatalf("complete recording command vocabulary was rejected: %v", err)
	}
	clipLandingRaw, err := json.Marshal(clipLanding)
	if err != nil {
		t.Fatal(err)
	}
	clipLandingFields, _, err := deriveCommandMetadata("recording.commit", clipLandingRaw, true)
	expectedClipLandingFields := []string{
		"clip:" + clipID.String() + ":asset",
		"clip:" + clipID.String() + ":automationActive",
		"clip:" + clipID.String() + ":automationDefaultValue",
		"clip:" + clipID.String() + ":automationTarget",
		"clip:" + clipID.String() + ":color",
		"clip:" + clipID.String() + ":compCrossfadeMs",
		"clip:" + clipID.String() + ":descendants",
		"clip:" + clipID.String() + ":durationSeconds",
		"clip:" + clipID.String() + ":fadeInCurve",
		"clip:" + clipID.String() + ":fadeInMode",
		"clip:" + clipID.String() + ":fadeInSeconds",
		"clip:" + clipID.String() + ":fadeOutCurve",
		"clip:" + clipID.String() + ":fadeOutMode",
		"clip:" + clipID.String() + ":fadeOutSeconds",
		"clip:" + clipID.String() + ":gain",
		"clip:" + clipID.String() + ":lifecycle",
		"clip:" + clipID.String() + ":musicalAnalysis",
		"clip:" + clipID.String() + ":muted",
		"clip:" + clipID.String() + ":name",
		"clip:" + clipID.String() + ":offsetSeconds",
		"clip:" + clipID.String() + ":pan",
		"clip:" + clipID.String() + ":patternClipId",
		"clip:" + clipID.String() + ":position",
		"clip:" + clipID.String() + ":sampleEdit",
		"clip:" + clipID.String() + ":startSeconds",
		"compSegment:" + segmentID.String() + ":endSeconds",
		"compSegment:" + segmentID.String() + ":position",
		"compSegment:" + segmentID.String() + ":startSeconds",
		"compSegment:" + segmentID.String() + ":takeId",
		"project:tempoCascade",
		"take:" + takeID.String() + ":clipOffsetSeconds",
		"take:" + takeID.String() + ":color",
		"take:" + takeID.String() + ":gain",
		"take:" + takeID.String() + ":lengthSeconds",
		"take:" + takeID.String() + ":lifecycle",
		"take:" + takeID.String() + ":muted",
		"take:" + takeID.String() + ":name",
		"take:" + takeID.String() + ":offsetSeconds",
		"take:" + takeID.String() + ":position",
		"track:" + trackA.String() + ":clipLanding",
	}
	if err != nil || !equalStrings(clipLandingFields, expectedClipLandingFields) {
		t.Fatalf("complete recording metadata = %v, want %v (%v)",
			clipLandingFields, expectedClipLandingFields, err)
	}
	tests := []map[string]any{
		{
			"leases":   []any{},
			"commands": []any{take(trackA)},
		},
		{
			"leases":   []any{map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()}},
			"commands": []any{},
		},
		{
			"leases": []any{
				map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()},
				map[string]any{"trackId": trackA.String(), "leaseId": leaseB.String()},
			},
			"commands": []any{take(trackA)},
		},
		{
			"leases": []any{
				map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()},
				map[string]any{"trackId": trackB.String(), "leaseId": leaseA.String()},
			},
			"commands": []any{take(trackA), take(trackB)},
		},
		{
			"leases":   []any{map[string]any{"trackId": strings.ToUpper(trackA.String()), "leaseId": leaseA.String()}},
			"commands": []any{take(trackA)},
		},
		{
			"leases":   []any{map[string]any{"trackId": trackB.String(), "leaseId": leaseB.String()}},
			"commands": []any{take(trackA)},
		},
		{
			"leases": []any{map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()}},
			"commands": []any{
				take(trackA),
				map[string]any{
					"kind": "track.setProperty",
					"payload": map[string]any{
						"trackId": trackA.String(), "property": "name", "value": "smuggled",
					},
					"preconditions": []any{},
				},
			},
		},
		{
			"leases": []any{map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()}},
			"commands": []any{
				take(trackA),
				map[string]any{
					"kind":          "project.setScalar",
					"payload":       map[string]any{"field": "tempo", "value": 140},
					"preconditions": []any{},
				},
			},
		},
		{
			"leases": []any{map[string]any{"trackId": trackA.String(), "leaseId": leaseA.String()}},
			"commands": []any{map[string]any{
				"kind": "batch",
				"payload": map[string]any{
					"commands": []any{take(trackA)},
				},
				"preconditions": []any{},
			}},
		},
	}
	for index, body := range tests {
		if err := validate(body); !errors.Is(err, ErrValidation) {
			t.Fatalf("invalid recording commit %d returned %v", index, err)
		}
	}
}

func TestRecordingCommitV3AllowsOnlyLeaseFreeNewClips(t *testing.T) {
	trackID, clipID, assetID := uuid.New(), uuid.New(), uuid.New()
	add := map[string]any{
		"kind": "clip.add",
		"payload": map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"clipKind": "audio", "name": "Concurrent take",
			"startSeconds": 2.0, "durationSeconds": 1.0, "color": 0,
		},
		"preconditions": []any{},
	}
	setAsset := map[string]any{
		"kind": "clip.setAsset",
		"payload": map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"asset": map[string]any{
				"assetId": assetID.String(), "sha256": strings.Repeat("d", 64),
				"kind": "audio", "byteSize": 512, "originalName": "take.wav",
			},
		},
		"preconditions": []any{},
	}
	validateV3 := func(commands []any) error {
		raw, err := json.Marshal(map[string]any{"leases": []any{}, "commands": commands})
		if err != nil {
			t.Fatal(err)
		}
		return validateCommandPayloadShapeForSchema("recording.commit", raw, true,
			CollaborationCommandSchemaV3)
	}
	if err := validateV3([]any{add, setAsset}); err != nil {
		t.Fatalf("lease-free v3 new clip was rejected: %v", err)
	}
	if err := validateV3([]any{setAsset}); !errors.Is(err, ErrValidation) {
		t.Fatalf("lease-free mutation of an existing clip returned %v", err)
	}
	otherClip := map[string]any{
		"kind": "clip.setProperty",
		"payload": map[string]any{
			"trackId": trackID.String(), "clipId": uuid.NewString(),
			"property": "gain", "value": 0.5,
		},
		"preconditions": []any{},
	}
	if err := validateV3([]any{add, otherClip}); !errors.Is(err, ErrValidation) {
		t.Fatalf("lease-free cross-clip mutation returned %v", err)
	}
}

func TestRecordingCommitBaseSequenceAllowsSafeRebaseCandidate(t *testing.T) {
	if err := validateOperationBaseSeq("recording.commit", 12, 12); err != nil {
		t.Fatalf("exact recording base was rejected: %v", err)
	}
	if err := validateOperationBaseSeq("recording.commit", 11, 12); err != nil {
		t.Fatalf("potentially rebasable recording base returned %v", err)
	}
	if err := validateOperationBaseSeq("recording.commit", 13, 12); !errors.Is(err, ErrBaseSeqAhead) {
		t.Fatalf("ahead recording base returned %v", err)
	}
	if err := validateOperationBaseSeq("track.setProperty", 11, 12); err != nil {
		t.Fatalf("ordinary stale scalar edit was rejected: %v", err)
	}
	if err := validateOperationBaseSeq("track.setProperty", 13, 12); !errors.Is(err, ErrBaseSeqAhead) {
		t.Fatalf("ordinary ahead base returned %v", err)
	}
}

func TestLeaseFreeRecordingCommitCanRebaseOverConcurrentInsert(t *testing.T) {
	store := &Store{}
	if err := store.validateRecordingCommitRebaseTx(nil, uuid.New(),
		"recording.commit", 10, 12, nil); err != nil {
		t.Fatalf("lease-free new-clip commit rejected safe rebase: %v", err)
	}
}

func TestAbsentOperationStatusJSONIsStable(t *testing.T) {
	encoded, err := json.Marshal(OperationStatus{Found: false, HeadSeq: 17})
	if err != nil {
		t.Fatal(err)
	}
	const expected = `{"found":false,"head_seq":17,"operation":null}`
	if string(encoded) != expected {
		t.Fatalf("absent operation status = %s", encoded)
	}
}

func TestRecordingCommitRebaseUsesTargetTrackLandingHeads(t *testing.T) {
	targetTrack := uuid.MustParse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")
	otherTrack := uuid.MustParse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")
	clipID := uuid.MustParse("cccccccc-cccc-4ccc-8ccc-cccccccccccc")
	leases := []recordingLeaseReference{{TrackID: targetTrack, LeaseID: uuid.New()}}
	field := func(key string, sequence int64) model.ProjectFieldHead {
		return model.ProjectFieldHead{FieldKey: key, HeadSeq: sequence}
	}

	allowed := []model.ProjectFieldHead{
		field("project:masterVolume", 12),
		field("track:"+targetTrack.String()+":volume", 12),
		field("track:"+otherTrack.String()+":lifecycle", 12),
		field("track:"+otherTrack.String()+":clipLanding", 12),
		field("clip:"+clipID.String()+":gain", 12),
		field("clip:"+clipID.String()+":name", 12),
		field("clip:"+clipID.String()+":descendants", 12),
		// Entity fields are not queried directly by rebase. The server-derived
		// target-track landing head below is their authoritative scope index.
		field("clip:"+clipID.String()+":asset", 10),
	}
	if err := validateRecordingCommitRebaseHeads(10, leases, allowed); err != nil {
		t.Fatalf("geometry-independent heads blocked safe rebase: %v", err)
	}

	if err := validateRecordingCommitRebaseHeads(10, leases,
		[]model.ProjectFieldHead{field(
			"track:"+targetTrack.String()+":clipLanding", 11)}); !errors.Is(err, ErrBaseSeqMismatch) {
		t.Fatalf("advanced target landing head returned %v", err)
	}

	if err := validateRecordingCommitRebaseHeads(10, leases,
		[]model.ProjectFieldHead{field(
			"track:"+targetTrack.String()+":lifecycle", 11)}); !errors.Is(err, ErrBaseSeqMismatch) {
		t.Fatalf("advanced target track lifecycle returned %v", err)
	}
	if err := validateRecordingCommitRebaseHeads(10, leases,
		[]model.ProjectFieldHead{field(
			recordingClipTrackAssignmentHead, 11)}); !errors.Is(err, ErrBaseSeqMismatch) {
		t.Fatalf("advanced clip assignment head returned %v", err)
	}
}

func TestEvaluateCommandLeasePolicy(t *testing.T) {
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	member := model.ProjectSessionMember{ID: uuid.New(), SessionID: uuid.New()}
	trackA := uuid.MustParse("11111111-1111-4111-8111-111111111111")
	trackB := uuid.MustParse("22222222-2222-4222-8222-222222222222")
	trackC := uuid.MustParse("33333333-3333-4333-8333-333333333333")
	leaseA := model.ProjectTrackLease{
		ID: uuid.New(), SessionID: member.SessionID, TrackID: trackA,
		LeaseKind: model.TrackLeaseRecord, HolderMemberID: member.ID, ExpiresAt: now.Add(time.Minute),
	}
	leaseB := model.ProjectTrackLease{
		ID: uuid.New(), SessionID: member.SessionID, TrackID: trackB,
		LeaseKind: model.TrackLeaseRecord, HolderMemberID: member.ID, ExpiresAt: now.Add(time.Minute),
	}
	releases, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		RecordingLeases: []recordingLeaseReference{
			{TrackID: trackB, LeaseID: leaseB.ID},
			{TrackID: trackA, LeaseID: leaseA.ID},
		},
	}, member, []model.ProjectTrackLease{leaseB, leaseA}, now)
	released := make(map[uuid.UUID]bool, len(releases))
	for _, leaseID := range releases {
		released[leaseID] = true
	}
	if err != nil || len(releases) != 2 || !released[leaseA.ID] || !released[leaseB.ID] {
		t.Fatalf("owned lease evaluation = %v, %v", releases, err)
	}
	if _, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		RecordingLeases: []recordingLeaseReference{
			{TrackID: trackA, LeaseID: leaseA.ID},
			{TrackID: trackC, LeaseID: uuid.New()},
		},
	}, member, []model.ProjectTrackLease{leaseA}, now); !errors.Is(err, ErrLeaseRequired) {
		t.Fatalf("missing record lease returned %v", err)
	}
	other := leaseA
	other.HolderMemberID = uuid.New()
	if _, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		RecordingLeases: []recordingLeaseReference{{TrackID: trackA, LeaseID: other.ID}},
	}, member, []model.ProjectTrackLease{other}, now); !errors.Is(err, ErrLeaseHeld) {
		t.Fatalf("foreign record lease returned %v", err)
	}
	if _, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		DeletedTrackIDs: []uuid.UUID{trackA},
	}, member, []model.ProjectTrackLease{leaseA}, now); !errors.Is(err, ErrLeaseHeld) {
		t.Fatalf("leased track delete returned %v", err)
	}
	if _, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		BlocksProjectTiming: true,
	}, member, []model.ProjectTrackLease{leaseA}, now); !errors.Is(err, ErrLeaseHeld) {
		t.Fatalf("timing change during recording returned %v", err)
	}
	expired := leaseA
	expired.ExpiresAt = now
	if _, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		RecordingLeases: []recordingLeaseReference{{TrackID: trackA, LeaseID: expired.ID}},
	}, member, []model.ProjectTrackLease{expired}, now); !errors.Is(err, ErrLeaseExpired) {
		t.Fatalf("expired record lease returned %v", err)
	}
	if _, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		RecordingLeases: []recordingLeaseReference{{TrackID: trackA, LeaseID: uuid.New()}},
	}, member, []model.ProjectTrackLease{leaseA}, now); !errors.Is(err, ErrLeaseRequired) {
		t.Fatalf("stale lease identifier returned %v", err)
	}
	otherSession := leaseA
	otherSession.SessionID = uuid.New()
	if _, err := evaluateCommandLeasePolicy(commandLeasePolicy{
		RecordingLeases: []recordingLeaseReference{{TrackID: trackA, LeaseID: otherSession.ID}},
	}, member, []model.ProjectTrackLease{otherSession}, now); !errors.Is(err, ErrLeaseRequired) {
		t.Fatalf("wrong-session lease returned %v", err)
	}
}

func TestDeriveExtendedCommandFields(t *testing.T) {
	trackID, clipID, noteID, pointID, deleteOperationID := uuid.New(), uuid.New(), uuid.New(), uuid.New(), uuid.New()
	tests := []struct {
		kind     string
		payload  map[string]any
		expected []string
	}{
		{
			kind: "clip.add",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(), "clipKind": "midi",
				"name": "Phrase", "startSeconds": 1.0, "durationSeconds": 4.0, "color": 7,
			},
			expected: []string{
				"clip:" + clipID.String() + ":asset",
				"clip:" + clipID.String() + ":automationActive",
				"clip:" + clipID.String() + ":automationDefaultValue",
				"clip:" + clipID.String() + ":automationTarget",
				"clip:" + clipID.String() + ":color",
				"clip:" + clipID.String() + ":compCrossfadeMs",
				"clip:" + clipID.String() + ":descendants",
				"clip:" + clipID.String() + ":durationSeconds",
				"clip:" + clipID.String() + ":fadeInCurve",
				"clip:" + clipID.String() + ":fadeInMode",
				"clip:" + clipID.String() + ":fadeInSeconds",
				"clip:" + clipID.String() + ":fadeOutCurve",
				"clip:" + clipID.String() + ":fadeOutMode",
				"clip:" + clipID.String() + ":fadeOutSeconds",
				"clip:" + clipID.String() + ":gain",
				"clip:" + clipID.String() + ":lifecycle",
				"clip:" + clipID.String() + ":musicalAnalysis",
				"clip:" + clipID.String() + ":muted",
				"clip:" + clipID.String() + ":name",
				"clip:" + clipID.String() + ":offsetSeconds",
				"clip:" + clipID.String() + ":pan",
				"clip:" + clipID.String() + ":patternClipId",
				"clip:" + clipID.String() + ":position",
				"clip:" + clipID.String() + ":sampleEdit",
				"clip:" + clipID.String() + ":startSeconds",
				"project:tempoCascade",
				"track:" + trackID.String() + ":clipLanding",
			},
		},
		{
			kind: "clip.setProperty",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(), "property": "muted", "value": true,
			},
			expected: []string{"clip:" + clipID.String() + ":muted"},
		},
		{
			kind: "clip.delete",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(),
			},
			expected: []string{
				"clip:" + clipID.String() + ":descendants",
				"clip:" + clipID.String() + ":lifecycle",
				"project:tempoCascade",
				"track:" + trackID.String() + ":clipLanding",
			},
		},
		{
			kind: "clip.restore",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(),
				"deleteOperationId": deleteOperationID.String(),
			},
			expected: []string{
				"clip:" + clipID.String() + ":descendants",
				"clip:" + clipID.String() + ":lifecycle",
				"clip:" + clipID.String() + ":position",
				"project:tempoCascade",
				"track:" + trackID.String() + ":clipLanding",
			},
		},
		{
			kind: "note.upsert",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(),
				"note": map[string]any{
					"id": noteID.String(), "pitch": 60, "startBeats": 1.0,
					"lengthBeats": 0.5, "velocity": 100, "muted": false,
					"color": 0, "pan": 0.0,
				},
			},
			expected: []string{
				"clip:" + clipID.String() + ":descendants",
				"note:" + noteID.String() + ":color", "note:" + noteID.String() + ":lengthBeats",
				"note:" + noteID.String() + ":muted", "note:" + noteID.String() + ":pan",
				"note:" + noteID.String() + ":pitch", "note:" + noteID.String() + ":position",
				"note:" + noteID.String() + ":startBeats", "note:" + noteID.String() + ":velocity",
			},
		},
		{
			kind: "automationPoint.delete",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(), "pointId": pointID.String(),
			},
			expected: []string{
				"automationPoint:" + pointID.String() + ":lifecycle",
				"clip:" + clipID.String() + ":descendants",
			},
		},
	}
	for _, test := range tests {
		payload, err := json.Marshal(test.payload)
		if err != nil {
			t.Fatal(err)
		}
		fields, _, err := deriveCommandMetadata(test.kind, payload, true)
		if err != nil {
			t.Fatalf("%s was rejected: %v", test.kind, err)
		}
		if !equalStrings(fields, test.expected) {
			t.Fatalf("%s fields = %v, want %v", test.kind, fields, test.expected)
		}
	}
}

func TestLifecycleStepsAndBatchEffect(t *testing.T) {
	trackID, deleteOperationID := uuid.New(), uuid.New()
	restorePayload := json.RawMessage(`{"trackId":"` + trackID.String() + `","deleteOperationId":"` + deleteOperationID.String() + `"}`)
	steps, err := deriveLifecycleSteps("track.restore", restorePayload, true)
	if err != nil || len(steps) != 1 || len(steps[0].Requirements) != 1 ||
		steps[0].Requirements[0].ExpectedDeleteOperationID == nil ||
		*steps[0].Requirements[0].ExpectedDeleteOperationID != deleteOperationID ||
		len(steps[0].Mutations) != 1 || steps[0].Mutations[0].Effect != lifecycleAlive {
		t.Fatalf("restore lifecycle step = %#v, %v", steps, err)
	}
	batchPayload, err := json.Marshal(map[string]any{"commands": []any{map[string]any{
		"kind": "track.delete", "payload": map[string]any{"trackId": trackID.String()}, "preconditions": []any{},
	}}})
	if err != nil {
		t.Fatal(err)
	}
	effect, found, err := lifecycleEffectForOperation("batch", batchPayload,
		"track:"+trackID.String()+":lifecycle")
	if err != nil || !found || effect != lifecycleDeleted {
		t.Fatalf("batch lifecycle effect = %q, %v, %v", effect, found, err)
	}
}

func TestStrictCommandPayloadValidation(t *testing.T) {
	trackID, clipID, noteID, takeID, assetID := uuid.New(), uuid.New(), uuid.New(), uuid.New(), uuid.New()
	validTake := map[string]any{
		"id": takeID.String(), "name": "Take", "offsetSeconds": 0.0,
		"lengthSeconds": 1.0, "clipOffsetSeconds": 0.0, "gain": 1.0,
		"muted": false, "channels": 2, "color": 0,
		"asset": map[string]any{
			"assetId": assetID.String(), "sha256": strings.Repeat("a", 64),
			"kind": "audio", "byteSize": 128, "originalName": "take.wav",
		},
	}
	valid := []struct {
		kind    string
		payload map[string]any
	}{
		{"controllerLane.add", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(), "laneId": uuid.NewString(),
			"name": "Mod", "target": map[string]any{"cc": 1, "parameterId": "", "slotId": ""},
			"defaultValue": 0.5, "afterId": "",
		}},
		{"automation.setTarget", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"target": map[string]any{
				"kind": "parameter", "channelId": trackID.String(), "slotId": "",
				"parameterId": "cutoff", "sendId": "",
			},
		}},
		{"take.add", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(), "take": validTake, "afterId": "",
		}},
		{"compSegment.upsert", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"segment": map[string]any{
				"id": uuid.NewString(), "takeId": takeID.String(), "startSeconds": 0.0, "endSeconds": 0.001,
			},
			"afterId": "",
		}},
		{"clip.setProperty", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"property": "compCrossfadeMs", "value": 7.25,
		}},
	}
	for _, test := range valid {
		payload, err := json.Marshal(test.payload)
		if err != nil {
			t.Fatal(err)
		}
		if err := validateCommandPayloadShape(test.kind, payload, true); err != nil {
			t.Fatalf("valid %s payload was rejected: %v", test.kind, err)
		}
	}

	invalid := []struct {
		kind    string
		payload map[string]any
	}{
		{"clip.add", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(), "clipKind": "audio",
			"name": "bad", "startSeconds": 0, "durationSeconds": -1, "color": 0,
		}},
		{"track.add", map[string]any{
			"trackId": trackID.String(), "trackKind": "unknown", "name": "bad", "color": 0,
			"parentId": "", "afterId": "",
		}},
		{"project.setScalar", map[string]any{"field": "tempo", "value": 120, "extra": true}},
		{"note.upsert", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(), "note": map[string]any{"id": noteID.String()},
		}},
		{"controllerLane.setTarget", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(), "laneId": uuid.NewString(),
			"target": map[string]any{"cc": -1, "parameterId": "", "slotId": ""},
		}},
		{"automation.setTarget", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"target": map[string]any{
				"kind": "send", "channelId": trackID.String(), "slotId": "",
				"parameterId": "", "sendId": "",
			},
		}},
		{"take.add", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(), "afterId": "",
			"take": func() map[string]any {
				copy := make(map[string]any, len(validTake))
				for key, value := range validTake {
					copy[key] = value
				}
				copy["filePath"] = "/private/audio.wav"
				return copy
			}(),
		}},
		{"compSegment.upsert", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"segment": map[string]any{
				"id": uuid.NewString(), "takeId": takeID.String(), "startSeconds": 1.0, "endSeconds": 1.0005,
			},
			"afterId": "",
		}},
		{"clip.setProperty", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"property": "compCrossfadeMs", "value": 20.001,
		}},
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

func TestOrderedBatchLifecycleSimulation(t *testing.T) {
	trackID, clipID, outerOperationID := uuid.New(), uuid.New(), uuid.New()
	batch := func(commands ...map[string]any) json.RawMessage {
		payload, err := json.Marshal(map[string]any{"commands": commands})
		if err != nil {
			t.Fatal(err)
		}
		return payload
	}
	child := func(kind string, payload map[string]any) map[string]any {
		return map[string]any{"kind": kind, "payload": payload, "preconditions": []any{}}
	}

	deleteThenTarget := batch(
		child("track.delete", map[string]any{"trackId": trackID.String()}),
		child("track.setProperty", map[string]any{
			"trackId": trackID.String(), "property": "name", "value": "late",
		}),
	)
	steps, err := deriveLifecycleSteps("batch", deleteThenTarget, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := validateLifecycleSequence(map[string]lifecycleState{}, outerOperationID, steps); !errors.Is(err, ErrEntityDeleted) {
		t.Fatalf("delete then target returned %v", err)
	}

	deleteParentThenClipTarget := batch(
		child("track.delete", map[string]any{"trackId": trackID.String()}),
		child("clip.setProperty", map[string]any{
			"trackId": trackID.String(), "clipId": clipID.String(),
			"property": "muted", "value": true,
		}),
	)
	steps, err = deriveLifecycleSteps("batch", deleteParentThenClipTarget, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := validateLifecycleSequence(map[string]lifecycleState{}, outerOperationID, steps); !errors.Is(err, ErrEntityDeleted) {
		t.Fatalf("parent delete then child target returned %v", err)
	}

	deleteThenRestore := batch(
		child("track.delete", map[string]any{"trackId": trackID.String()}),
		child("track.restore", map[string]any{
			"trackId": trackID.String(), "deleteOperationId": outerOperationID.String(),
		}),
		child("track.setProperty", map[string]any{
			"trackId": trackID.String(), "property": "name", "value": "restored",
		}),
	)
	steps, err = deriveLifecycleSteps("batch", deleteThenRestore, true)
	if err != nil {
		t.Fatal(err)
	}
	if err := validateLifecycleSequence(map[string]lifecycleState{}, outerOperationID, steps); err != nil {
		t.Fatalf("ordered delete/restore/target was rejected: %v", err)
	}
}

func TestDerivePhaseTwoCommandFields(t *testing.T) {
	trackID, clipID, laneID, takeID, segmentID, assetID :=
		uuid.New(), uuid.New(), uuid.New(), uuid.New(), uuid.New(), uuid.New()
	tests := []struct {
		kind     string
		payload  map[string]any
		expected []string
	}{
		{
			kind: "controllerLane.add",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(), "laneId": laneID.String(),
				"name": "CC", "target": map[string]any{"cc": 1, "parameterId": "", "slotId": ""},
				"defaultValue": 0.5, "afterId": "",
			},
			expected: []string{
				"clip:" + clipID.String() + ":descendants",
				"controllerLane:" + laneID.String() + ":lifecycle",
				"controllerLane:" + laneID.String() + ":position",
			},
		},
		{
			kind: "take.add",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(), "afterId": "",
				"take": map[string]any{
					"id": takeID.String(), "name": "Take", "offsetSeconds": 0.0,
					"lengthSeconds": 1.0, "clipOffsetSeconds": 0.0, "gain": 1.0,
					"muted": false, "channels": 2, "color": 0,
					"asset": map[string]any{
						"assetId": assetID.String(), "sha256": strings.Repeat("b", 64),
						"kind": "audio", "byteSize": 16, "originalName": "take.wav",
					},
				},
			},
			expected: []string{
				"clip:" + clipID.String() + ":descendants",
				"take:" + takeID.String() + ":clipOffsetSeconds",
				"take:" + takeID.String() + ":color",
				"take:" + takeID.String() + ":gain",
				"take:" + takeID.String() + ":lengthSeconds",
				"take:" + takeID.String() + ":lifecycle",
				"take:" + takeID.String() + ":muted",
				"take:" + takeID.String() + ":name",
				"take:" + takeID.String() + ":offsetSeconds",
				"take:" + takeID.String() + ":position",
			},
		},
		{
			kind: "compSegment.upsert",
			payload: map[string]any{
				"trackId": trackID.String(), "clipId": clipID.String(), "afterId": "",
				"segment": map[string]any{
					"id": segmentID.String(), "takeId": takeID.String(), "startSeconds": 0.0, "endSeconds": 1.0,
				},
			},
			expected: []string{
				"clip:" + clipID.String() + ":descendants",
				"compSegment:" + segmentID.String() + ":endSeconds",
				"compSegment:" + segmentID.String() + ":position",
				"compSegment:" + segmentID.String() + ":startSeconds",
				"compSegment:" + segmentID.String() + ":takeId",
			},
		},
	}
	for _, test := range tests {
		payload, err := json.Marshal(test.payload)
		if err != nil {
			t.Fatal(err)
		}
		fields, _, err := deriveCommandMetadata(test.kind, payload, true)
		if err != nil {
			t.Fatalf("%s was rejected: %v", test.kind, err)
		}
		if !equalStrings(fields, test.expected) {
			t.Fatalf("%s fields = %v, want %v", test.kind, fields, test.expected)
		}
	}
}

func TestCollaborationV1ScaleLimits(t *testing.T) {
	if MaxOperationPayloadBytes != 1<<20 || MaxOperationPreconditions != 1024 ||
		MaxOperationTouchedFields != 8192 || maxBatchCommands != 1024 {
		t.Fatalf("unexpected collaboration v2 limits: payload=%d preconditions=%d fields=%d batch=%d",
			MaxOperationPayloadBytes, MaxOperationPreconditions, MaxOperationTouchedFields, maxBatchCommands)
	}
}

func TestValidateCreateCompatibility(t *testing.T) {
	engine, minimum, err := validateCreateCompatibility(CollaborationProjectFormatVersion, " engine-1 ", " 1.2.3 ")
	if err != nil || engine != "engine-1" || minimum != "1.2.3" {
		t.Fatalf("valid compatibility = %q, %q, %v", engine, minimum, err)
	}
	for _, test := range []struct {
		format  int
		engine  string
		minimum string
	}{
		{CollaborationProjectFormatVersion - 1, "engine-1", "1.2.3"},
		{CollaborationProjectFormatVersion, "", "1.2.3"},
		{CollaborationProjectFormatVersion, "engine-1", ""},
		{CollaborationProjectFormatVersion, "engine-1", "release-2026"},
	} {
		if _, _, err := validateCreateCompatibility(test.format, test.engine, test.minimum); !errors.Is(err, ErrValidation) {
			t.Fatalf("invalid compatibility was accepted: %#v (%v)", test, err)
		}
	}
}

func TestValidateClientCompatibility(t *testing.T) {
	project := model.CloudProject{
		FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-42", MinimumAppVersion: "1.2.3-beta.2",
	}
	compatible := ClientCompatibility{
		AppVersion: "1.2.3-beta.10+build.7", EngineVersion: "engine-42",
		CommandSchemaVersion: CollaborationCommandSchemaVersion,
		ProjectFormatVersion: CollaborationProjectFormatVersion,
	}
	if err := ValidateClientCompatibility(project, compatible); err != nil {
		t.Fatalf("compatible client was rejected: %v", err)
	}

	tests := []ClientCompatibility{
		func() ClientCompatibility {
			value := compatible
			value.AppVersion = "1.2.3-beta.1"
			return value
		}(),
		func() ClientCompatibility {
			value := compatible
			value.AppVersion = "1.02.3"
			return value
		}(),
		func() ClientCompatibility {
			value := compatible
			value.EngineVersion = "engine-41"
			return value
		}(),
		func() ClientCompatibility {
			value := compatible
			value.ProjectFormatVersion++
			return value
		}(),
	}
	for _, client := range tests {
		if err := ValidateClientCompatibility(project, client); !errors.Is(err, ErrVersionMismatch) {
			t.Fatalf("incompatible client was accepted: %#v (%v)", client, err)
		}
	}

	project.MinimumAppVersion = "release-2026"
	if err := ValidateClientCompatibility(project, compatible); !errors.Is(err, ErrVersionMismatch) {
		t.Fatalf("invalid stored minimum failed open: %v", err)
	}
}

func TestSemanticVersionPrecedence(t *testing.T) {
	ordered := []string{
		"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
		"1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
		"1.0.0-rc.1", "1.0.0",
	}
	for index := 1; index < len(ordered); index++ {
		left, leftOK := parseSemanticVersion(ordered[index-1])
		right, rightOK := parseSemanticVersion(ordered[index])
		if !leftOK || !rightOK || compareSemanticVersions(left, right) >= 0 {
			t.Fatalf("semantic precedence is not increasing: %q, %q",
				ordered[index-1], ordered[index])
		}
	}
}

func TestLeaseTTLBounds(t *testing.T) {
	if ttl, err := normalizeLeaseTTL(0); err != nil || ttl != DefaultLeaseTTL {
		t.Fatalf("default TTL = %s, %v", ttl, err)
	}
	for _, ttl := range []time.Duration{MinLeaseTTL, MaxLeaseTTL} {
		if got, err := normalizeLeaseTTL(ttl); err != nil || got != ttl {
			t.Fatalf("valid TTL %s was rejected: %s, %v", ttl, got, err)
		}
	}
	for _, ttl := range []time.Duration{MinLeaseTTL - time.Second, MaxLeaseTTL + time.Second} {
		if _, err := normalizeLeaseTTL(ttl); !errors.Is(err, ErrValidation) {
			t.Fatalf("invalid TTL %s was accepted: %v", ttl, err)
		}
	}
}

func TestValidateExpectedLeaseTrack(t *testing.T) {
	actual := uuid.New()
	if err := validateExpectedLeaseTrack(actual, uuid.Nil); err != nil {
		t.Fatalf("unscoped REST lease was rejected: %v", err)
	}
	if err := validateExpectedLeaseTrack(actual, actual); err != nil {
		t.Fatalf("matching WS lease track was rejected: %v", err)
	}
	if err := validateExpectedLeaseTrack(actual, uuid.New()); !errors.Is(err, ErrValidation) {
		t.Fatalf("mismatched WS lease track returned %v", err)
	}
}

func TestInviteExpiryReuseAndTarget(t *testing.T) {
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	email := "member@example.com"
	user := model.User{ID: uuid.New(), EmailKey: email}
	invite := model.ProjectInvite{ExpiresAt: now.Add(time.Hour), TargetEmailKey: &email}
	if err := inviteAcceptanceError(invite, user, now); err != nil {
		t.Fatalf("available invite was rejected: %v", err)
	}
	invite.ExpiresAt = now
	if err := inviteAcceptanceError(invite, user, now); !errors.Is(err, ErrInviteExpired) {
		t.Fatalf("expired invite returned %v", err)
	}
	invite.ExpiresAt = now.Add(time.Hour)
	accepted := now
	invite.AcceptedAt = &accepted
	if err := inviteAcceptanceError(invite, user, now); !errors.Is(err, ErrInviteUsed) {
		t.Fatalf("reused invite returned %v", err)
	}
	invite.AcceptedAt = nil
	user.EmailKey = "other@example.com"
	if err := inviteAcceptanceError(invite, user, now); !errors.Is(err, ErrForbidden) {
		t.Fatalf("targeted invite accepted the wrong account: %v", err)
	}
}
