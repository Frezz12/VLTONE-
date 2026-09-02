package collab

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
)

func takeAddFixtureTouchedFields(trackID, clipID, takeID uuid.UUID, recordingCommit bool) []string {
	fields := []string{
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
	}
	if recordingCommit {
		fields = append(fields, "track:"+trackID.String()+":clipLanding")
	}
	return fields
}

func TestTakeAddFixtureTouchedFieldsMatchContract(t *testing.T) {
	trackID, clipID, takeID, leaseID, assetID := uuid.New(), uuid.New(), uuid.New(), uuid.New(), uuid.New()
	takePayload := map[string]any{
		"trackId": trackID.String(), "clipId": clipID.String(), "afterId": "",
		"take": map[string]any{
			"id": takeID.String(), "name": "Take", "offsetSeconds": 0.0,
			"lengthSeconds": 1.0, "clipOffsetSeconds": 0.0, "gain": 1.0,
			"muted": false, "channels": 2, "color": 0,
			"asset": map[string]any{
				"assetId": assetID.String(), "sha256": strings.Repeat("d", 64),
				"kind": "audio", "byteSize": 512, "originalName": "take.wav",
			},
		},
	}
	encodedTake, err := json.Marshal(takePayload)
	if err != nil {
		t.Fatal(err)
	}
	takeFields, _, err := deriveCommandMetadata("take.add", encodedTake, true)
	if err != nil {
		t.Fatal(err)
	}
	if expected := takeAddFixtureTouchedFields(trackID, clipID, takeID, false); !equalStrings(takeFields, expected) {
		t.Fatalf("take.add fields = %v, fixture = %v", takeFields, expected)
	}

	encodedCommit, err := json.Marshal(map[string]any{
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
	commitFields, _, err := deriveCommandMetadata("recording.commit", encodedCommit, true)
	if err != nil {
		t.Fatal(err)
	}
	if expected := takeAddFixtureTouchedFields(trackID, clipID, takeID, true); !equalStrings(commitFields, expected) {
		t.Fatalf("recording.commit fields = %v, fixture = %v", commitFields, expected)
	}
}

// The opt-in PostgreSQL test verifies the transactional boundary that unit
// policy tests cannot cover: sequence allocation and exact lease consumption
// either commit together or neither does, and an idempotent retry does not
// require (or consume) the already-deleted lease a second time.
func TestPostgresRecordingCommitConsumesExactLeaseOnce(t *testing.T) {
	dsn := os.Getenv("VLT_COLLAB_TEST_DATABASE_URL")
	if dsn == "" {
		if os.Getenv("CI") != "" {
			t.Fatal("VLT_COLLAB_TEST_DATABASE_URL is required in CI")
		}
		t.Skip("set VLT_COLLAB_TEST_DATABASE_URL to run collaboration PostgreSQL tests")
	}
	db, err := database.Open(dsn, false)
	if err != nil {
		t.Fatal(err)
	}
	sqlDB, _ := db.DB()
	t.Cleanup(func() { _ = sqlDB.Close() })
	tx := db.Begin()
	if tx.Error != nil {
		t.Fatal(tx.Error)
	}
	t.Cleanup(func() { _ = tx.Rollback().Error })

	now := time.Date(2026, 8, 31, 12, 0, 0, 0, time.UTC)
	actor := createPublicationActor(t, tx, now, "recording-commit")
	project := model.CloudProject{
		ID: uuid.New(), OwnerUserID: actor.user.ID, Title: "Recording commit test",
		Status: model.ProjectActive, FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-test", MinimumAppVersion: "1.0.0",
		CreatedAt: now, UpdatedAt: now,
	}
	if err := tx.Create(&project).Error; err != nil {
		t.Fatal(err)
	}
	started := now.Add(-time.Minute)
	session := model.ProjectSession{
		ID: uuid.New(), ProjectID: project.ID, CreatedBy: &actor.user.ID,
		Mode: model.SessionModeIndependent, Status: model.ProjectSessionActive,
		Version: 1, CreatedAt: started, StartedAt: &started, UpdatedAt: now,
	}
	if err := tx.Create(&session).Error; err != nil {
		t.Fatal(err)
	}
	member := model.ProjectSessionMember{
		ID: uuid.New(), SessionID: session.ID, UserID: actor.user.ID,
		DeviceID: actor.device.ID, DesktopSessionID: &actor.session.ID,
		JoinedAt: started, LastSeenAt: now,
	}
	if err := tx.Create(&member).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).
		Update("host_member_id", member.ID).Error; err != nil {
		t.Fatal(err)
	}

	trackID, clipID, takeID := uuid.New(), uuid.New(), uuid.New()
	lease := model.ProjectTrackLease{
		ID: uuid.New(), ProjectID: project.ID, SessionID: session.ID,
		TrackID: trackID, LeaseKind: model.TrackLeaseRecord,
		HolderMemberID: member.ID, AcquiredAt: now.Add(-10 * time.Second),
		RenewedAt: now.Add(-5 * time.Second), ExpiresAt: now.Add(time.Minute),
	}
	if err := tx.Create(&lease).Error; err != nil {
		t.Fatal(err)
	}
	assetID := uuid.New()
	digest := strings.Repeat("d", 64)
	verifiedAt := now
	blob := model.Blob{
		ID: uuid.New(), SHA256: digest, Bytes: 512,
		ContentType: "audio/wav", Kind: "audio",
		ObjectKey: "blobs/" + uuid.NewString(), Status: BlobReady,
		CreatedBy: &actor.user.ID, CreatedAt: now, VerifiedAt: &verifiedAt,
	}
	if err := tx.Create(&blob).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Create(&model.ProjectAsset{
		ProjectID: project.ID, AssetID: assetID, BlobID: blob.ID,
		Kind: "audio", DisplayName: "recording.wav",
		CreatedBy: &actor.user.ID, CreatedAt: now,
	}).Error; err != nil {
		t.Fatal(err)
	}
	clipAssetPayload := func(asset any) map[string]any {
		return map[string]any{
			"commands": []any{map[string]any{
				"kind": "clip.setAsset",
				"payload": map[string]any{
					"trackId": trackID.String(), "clipId": clipID.String(), "asset": asset,
				},
				"preconditions": []any{},
			}},
		}
	}
	readyAssetRef := map[string]any{
		"assetId": assetID.String(), "sha256": digest, "kind": "audio",
		"byteSize": 512, "originalName": "recording.wav",
	}
	checkTrigger := func(payload map[string]any) bool {
		t.Helper()
		raw, err := json.Marshal(payload)
		if err != nil {
			t.Fatal(err)
		}
		var ready bool
		if err := tx.Raw(`SELECT collaboration_command_assets_ready(?, ?, CAST(? AS jsonb))`,
			project.ID, "recording.commit", string(raw)).Scan(&ready).Error; err != nil {
			t.Fatal(err)
		}
		return ready
	}
	if !checkTrigger(clipAssetPayload(readyAssetRef)) {
		t.Fatal("recording.commit trigger rejected a ready clip.setAsset")
	}
	missingAssetRef := map[string]any{
		"assetId": uuid.NewString(), "sha256": digest, "kind": "audio",
		"byteSize": 512, "originalName": "missing.wav",
	}
	if checkTrigger(clipAssetPayload(missingAssetRef)) {
		t.Fatal("recording.commit trigger accepted an unavailable clip.setAsset")
	}
	malformedAssetRef := map[string]any{
		"assetId": assetID.String(), "sha256": digest,
		"byteSize": 512, "originalName": "malformed.wav",
	}
	if checkTrigger(clipAssetPayload(malformedAssetRef)) {
		t.Fatal("recording.commit trigger accepted a malformed clip AssetRef")
	}
	if !checkTrigger(clipAssetPayload(nil)) {
		t.Fatal("recording.commit trigger rejected an explicit null clip asset")
	}
	takePayload := map[string]any{
		"trackId": trackID.String(), "clipId": clipID.String(), "afterId": "",
		"take": map[string]any{
			"id": takeID.String(), "name": "Take", "offsetSeconds": 0.0,
			"lengthSeconds": 1.0, "clipOffsetSeconds": 0.0, "gain": 1.0,
			"muted": false, "channels": 2, "color": 0,
			"asset": map[string]any{
				"assetId": assetID.String(), "sha256": digest, "kind": "audio",
				"byteSize": 512, "originalName": "recording.wav",
			},
		},
	}
	payload, err := json.Marshal(map[string]any{
		"leases": []any{map[string]any{
			"trackId": trackID.String(), "leaseId": lease.ID.String(),
		}},
		"commands": []any{map[string]any{
			"kind": "take.add", "payload": takePayload, "preconditions": []any{},
		}},
	})
	if err != nil {
		t.Fatal(err)
	}
	input := AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "recording.commit", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: 0, Payload: payload,
		TouchedFields: takeAddFixtureTouchedFields(trackID, clipID, takeID, true),
	}
	store := NewStore(tx, 8)
	store.Now = func() time.Time { return now }
	operation, duplicate, err := store.AppendOperation(context.Background(), input)
	if err != nil || duplicate || operation.Seq != 1 {
		t.Fatalf("recording commit = seq %d duplicate %v err %v", operation.Seq, duplicate, err)
	}
	var leaseCount int64
	if err := tx.Model(&model.ProjectTrackLease{}).Where("id = ?", lease.ID).
		Count(&leaseCount).Error; err != nil || leaseCount != 0 {
		t.Fatalf("consumed lease count = %d, err %v", leaseCount, err)
	}
	retry, duplicate, err := store.AppendOperation(context.Background(), input)
	if err != nil || !duplicate || retry.Seq != operation.Seq {
		t.Fatalf("idempotent retry = seq %d duplicate %v err %v", retry.Seq, duplicate, err)
	}
	var operationCount int64
	if err := tx.Model(&model.ProjectOperation{}).
		Where("project_id = ? AND op_id = ?", project.ID, input.OpID).
		Count(&operationCount).Error; err != nil || operationCount != 1 {
		t.Fatalf("durable operation count = %d, err %v", operationCount, err)
	}
	var operationAssetRefs int64
	if err := tx.Model(&model.ProjectOperationAsset{}).
		Where("project_id = ? AND operation_seq = ? AND asset_id = ?",
			project.ID, operation.Seq, assetID).
		Count(&operationAssetRefs).Error; err != nil || operationAssetRefs != 1 {
		t.Fatalf("durable operation asset refs = %d, err %v", operationAssetRefs, err)
	}

	foundStatus, err := store.GetOperationStatus(context.Background(),
		project.ID, actor.user.ID, input.OpID)
	if err != nil || !foundStatus.Found || foundStatus.HeadSeq != 1 ||
		foundStatus.Operation == nil || foundStatus.Operation.OpID != input.OpID ||
		foundStatus.Operation.Seq != 1 {
		t.Fatalf("found operation status = %#v, err %v", foundStatus, err)
	}
	absentStatus, err := store.GetOperationStatus(context.Background(),
		project.ID, actor.user.ID, uuid.New())
	if err != nil || absentStatus.Found || absentStatus.HeadSeq != 1 ||
		absentStatus.Operation != nil {
		t.Fatalf("absent operation status = %#v, err %v", absentStatus, err)
	}

	viewer := createPublicationActor(t, tx, now, "operation-status-viewer")
	inviter := actor.user.ID
	if err := tx.Create(&model.ProjectMember{
		ProjectID: project.ID, UserID: viewer.user.ID,
		Role: model.ProjectRoleViewer, ColorIndex: 2, InvitedBy: &inviter,
		JoinedAt: now, UpdatedAt: now,
	}).Error; err != nil {
		t.Fatal(err)
	}
	viewerStatus, err := store.GetOperationStatus(context.Background(),
		project.ID, viewer.user.ID, input.OpID)
	if err != nil || !viewerStatus.Found || viewerStatus.HeadSeq != 1 ||
		viewerStatus.Operation == nil || viewerStatus.Operation.OpID != input.OpID {
		t.Fatalf("viewer operation status = %#v, err %v", viewerStatus, err)
	}
	outsider := createPublicationActor(t, tx, now, "operation-status-outsider")
	if _, err := store.GetOperationStatus(context.Background(), project.ID,
		outsider.user.ID, input.OpID); !errors.Is(err, ErrForbidden) {
		t.Fatalf("nonmember operation status returned %v", err)
	}
	if _, err := store.GetOperationStatus(context.Background(), uuid.New(),
		actor.user.ID, input.OpID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("missing project operation status returned %v", err)
	}

	// A bare take.add remains an ordinary document operation. It must never
	// infer or consume a currently held recording lease from trackId alone.
	secondTrack, secondTake, secondLease := uuid.New(), uuid.New(), uuid.New()
	bareLease := model.ProjectTrackLease{
		ID: secondLease, ProjectID: project.ID, SessionID: session.ID,
		TrackID: secondTrack, LeaseKind: model.TrackLeaseRecord,
		HolderMemberID: member.ID, AcquiredAt: now, RenewedAt: now,
		ExpiresAt: now.Add(time.Minute),
	}
	if err := tx.Create(&bareLease).Error; err != nil {
		t.Fatal(err)
	}
	bareTakePayload := make(map[string]any, len(takePayload))
	for key, value := range takePayload {
		bareTakePayload[key] = value
	}
	bareTakePayload["trackId"] = secondTrack.String()
	takeBody := make(map[string]any)
	for key, value := range takePayload["take"].(map[string]any) {
		takeBody[key] = value
	}
	takeBody["id"] = secondTake.String()
	bareTakePayload["take"] = takeBody
	barePayload, err := json.Marshal(bareTakePayload)
	if err != nil {
		t.Fatal(err)
	}
	_, duplicate, err = store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "take.add", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: 1, Payload: barePayload,
		TouchedFields: takeAddFixtureTouchedFields(secondTrack, clipID, secondTake, false),
	})
	if err != nil || duplicate {
		t.Fatalf("bare take.add = duplicate %v err %v", duplicate, err)
	}
	if err := tx.Model(&model.ProjectTrackLease{}).Where("id = ?", bareLease.ID).
		Count(&leaseCount).Error; err != nil || leaseCount != 1 {
		t.Fatalf("bare take.add consumed lease: count %d err %v", leaseCount, err)
	}

	// A mixer edit and take metadata after the recording base do not alter
	// clip landing geometry. The stale recording commit may therefore take the
	// next current server sequence without rebuilding its document plan.
	mixerPayload, err := json.Marshal(map[string]any{
		"field": "masterVolume", "value": 0.8,
	})
	if err != nil {
		t.Fatal(err)
	}
	_, duplicate, err = store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "project.setScalar", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: 2, Payload: mixerPayload,
		TouchedFields: []string{"project:masterVolume"},
	})
	if err != nil || duplicate {
		t.Fatalf("mixer edit = duplicate %v err %v", duplicate, err)
	}

	commitTakeID := uuid.New()
	commitTakePayload := make(map[string]any, len(bareTakePayload))
	for key, value := range bareTakePayload {
		commitTakePayload[key] = value
	}
	commitTakeBody := make(map[string]any)
	for key, value := range bareTakePayload["take"].(map[string]any) {
		commitTakeBody[key] = value
	}
	commitTakeBody["id"] = commitTakeID.String()
	commitTakePayload["take"] = commitTakeBody
	rebasedPayload, err := json.Marshal(map[string]any{
		"leases": []any{map[string]any{
			"trackId": secondTrack.String(), "leaseId": bareLease.ID.String(),
		}},
		"commands": []any{map[string]any{
			"kind": "take.add", "payload": commitTakePayload, "preconditions": []any{},
		}},
	})
	if err != nil {
		t.Fatal(err)
	}
	rebased, duplicate, err := store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "recording.commit", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: 1, Payload: rebasedPayload,
		TouchedFields: takeAddFixtureTouchedFields(secondTrack, clipID, commitTakeID, true),
	})
	if err != nil || duplicate || rebased.Seq != 4 {
		t.Fatalf("safe rebased recording commit = seq %d duplicate %v err %v",
			rebased.Seq, duplicate, err)
	}
	if err := tx.Model(&model.ProjectTrackLease{}).Where("id = ?", bareLease.ID).
		Count(&leaseCount).Error; err != nil || leaseCount != 0 {
		t.Fatalf("safe rebased commit lease count %d err %v", leaseCount, err)
	}

	newLease := func(trackID uuid.UUID) model.ProjectTrackLease {
		lease := model.ProjectTrackLease{
			ID: uuid.New(), ProjectID: project.ID, SessionID: session.ID,
			TrackID: trackID, LeaseKind: model.TrackLeaseRecord,
			HolderMemberID: member.ID, AcquiredAt: now, RenewedAt: now,
			ExpiresAt: now.Add(time.Minute),
		}
		if err := tx.Create(&lease).Error; err != nil {
			t.Fatal(err)
		}
		return lease
	}
	commitFor := func(trackID, leaseID, targetClipID, targetTakeID uuid.UUID) (json.RawMessage, []string) {
		body := make(map[string]any)
		for key, value := range takePayload["take"].(map[string]any) {
			body[key] = value
		}
		body["id"] = targetTakeID.String()
		child := map[string]any{
			"trackId": trackID.String(), "clipId": targetClipID.String(),
			"afterId": "", "take": body,
		}
		encoded, err := json.Marshal(map[string]any{
			"leases": []any{map[string]any{
				"trackId": trackID.String(), "leaseId": leaseID.String(),
			}},
			"commands": []any{map[string]any{
				"kind": "take.add", "payload": child, "preconditions": []any{},
			}},
		})
		if err != nil {
			t.Fatal(err)
		}
		return encoded, takeAddFixtureTouchedFields(trackID, targetClipID, targetTakeID, true)
	}
	assertLeaseRemains := func(leaseID uuid.UUID, label string) {
		if err := tx.Model(&model.ProjectTrackLease{}).Where("id = ?", leaseID).
			Count(&leaseCount).Error; err != nil || leaseCount != 1 {
			t.Fatalf("%s lease count %d err %v", label, leaseCount, err)
		}
	}

	// Two recordings planned at one document head remain independent when
	// their exclusive leases target different tracks. The first commit advances
	// only its track-scoped landing head; the second may safely rebase over it.
	parallelBase := rebased.Seq
	parallelTrackA, parallelClipA, parallelTakeA := uuid.New(), uuid.New(), uuid.New()
	parallelTrackB, parallelClipB, parallelTakeB := uuid.New(), uuid.New(), uuid.New()
	parallelLeaseA := newLease(parallelTrackA)
	parallelLeaseB := newLease(parallelTrackB)
	parallelPayloadA, parallelFieldsA := commitFor(
		parallelTrackA, parallelLeaseA.ID, parallelClipA, parallelTakeA)
	parallelPayloadB, parallelFieldsB := commitFor(
		parallelTrackB, parallelLeaseB.ID, parallelClipB, parallelTakeB)
	parallelA, duplicate, err := store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "recording.commit", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: parallelBase, Payload: parallelPayloadA, TouchedFields: parallelFieldsA,
	})
	if err != nil || duplicate || parallelA.Seq != parallelBase+1 {
		t.Fatalf("first parallel-track recording = seq %d duplicate %v err %v",
			parallelA.Seq, duplicate, err)
	}
	parallelB, duplicate, err := store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "recording.commit", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: parallelBase, Payload: parallelPayloadB, TouchedFields: parallelFieldsB,
	})
	if err != nil || duplicate || parallelB.Seq != parallelBase+2 {
		t.Fatalf("stale second parallel-track recording = seq %d duplicate %v err %v",
			parallelB.Seq, duplicate, err)
	}
	if err := tx.Model(&model.ProjectTrackLease{}).
		Where("id IN ?", []uuid.UUID{parallelLeaseA.ID, parallelLeaseB.ID}).
		Count(&leaseCount).Error; err != nil || leaseCount != 0 {
		t.Fatalf("parallel-track recording leases = %d, err %v", leaseCount, err)
	}

	geometryTrack, geometryClip, geometryTake := uuid.New(), uuid.New(), uuid.New()
	geometryLease := newLease(geometryTrack)
	geometryPayload, geometryFields := commitFor(
		geometryTrack, geometryLease.ID, geometryClip, geometryTake)
	clipMovePayload, err := json.Marshal(map[string]any{
		"trackId": geometryTrack.String(), "clipId": geometryClip.String(),
		"property": "startSeconds", "value": 2.0,
	})
	if err != nil {
		t.Fatal(err)
	}
	geometryEdit, duplicate, err := store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "clip.setProperty", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: parallelB.Seq, Payload: clipMovePayload,
		TouchedFields: []string{
			"clip:" + geometryClip.String() + ":startSeconds",
			"project:tempoCascade",
			"track:" + geometryTrack.String() + ":clipLanding",
		},
	})
	if err != nil || duplicate || geometryEdit.Seq != parallelB.Seq+1 {
		t.Fatalf("clip geometry edit = seq %d duplicate %v err %v",
			geometryEdit.Seq, duplicate, err)
	}
	_, _, err = store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "recording.commit", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: parallelB.Seq, Payload: geometryPayload, TouchedFields: geometryFields,
	})
	if !errors.Is(err, ErrBaseSeqMismatch) {
		t.Fatalf("clip geometry race returned %v", err)
	}
	assertLeaseRemains(geometryLease.ID, "geometry-conflicted commit")

	lifecycleTrack, lifecycleClip, lifecycleTake := uuid.New(), uuid.New(), uuid.New()
	lifecycleLease := newLease(lifecycleTrack)
	lifecyclePayload, lifecycleFields := commitFor(
		lifecycleTrack, lifecycleLease.ID, lifecycleClip, lifecycleTake)
	trackAddPayload, err := json.Marshal(map[string]any{
		"trackId": lifecycleTrack.String(), "trackKind": "audio",
		"name": "Target", "color": 0, "parentId": "", "afterId": "",
	})
	if err != nil {
		t.Fatal(err)
	}
	lifecycleEdit, duplicate, err := store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "track.add", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: geometryEdit.Seq, Payload: trackAddPayload,
		TouchedFields: []string{
			"track:" + lifecycleTrack.String() + ":lifecycle",
			"track:" + lifecycleTrack.String() + ":position",
		},
	})
	if err != nil || duplicate || lifecycleEdit.Seq != geometryEdit.Seq+1 {
		t.Fatalf("target track lifecycle edit = seq %d duplicate %v err %v",
			lifecycleEdit.Seq, duplicate, err)
	}
	_, _, err = store.AppendOperation(context.Background(), AppendOperationInput{
		ProjectID: project.ID, ActorUserID: actor.user.ID,
		ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID,
		OpID: uuid.New(), Kind: "recording.commit", SchemaVersion: CollaborationCommandSchemaVersion,
		BaseSeq: geometryEdit.Seq, Payload: lifecyclePayload, TouchedFields: lifecycleFields,
	})
	if !errors.Is(err, ErrBaseSeqMismatch) {
		t.Fatalf("target track lifecycle race returned %v", err)
	}
	assertLeaseRemains(lifecycleLease.ID, "track-conflicted commit")
}
