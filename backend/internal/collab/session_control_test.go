package collab

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
)

func TestV6SessionControlOrderingAndValidation(t *testing.T) {
	control := ControlSnapshot(model.ProjectSession{Mode: model.SessionModeSynchronized, Version: 3})
	position := 10.0
	play := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: 3, Kind: "play", PositionSeconds: &position}
	if err := applyControl(&control, play, 1000); err != nil {
		t.Fatal(err)
	}
	stop := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: 3, Kind: "stop"}
	if err := applyControl(&control, stop, 1001); err != nil {
		t.Fatal(err)
	}
	if control.Transport.Playing || control.Transport.Revision != 2 || math.Abs(control.Transport.PositionSeconds-10.001) > .000001 {
		t.Fatalf("rapid stop lost: %+v", control)
	}
	track := uuid.New()
	enabled := true
	if err := applyControl(&control, ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: 3, Kind: "audition", TrackID: &track, Muted: &enabled, Solo: &enabled}, 1002); err != nil {
		t.Fatal(err)
	}
	if len(control.Audition.MutedTrackIDs) != 1 || len(control.Audition.SoloTrackIDs) != 1 || control.Transport.Revision != 2 {
		t.Fatal("audition changed transport")
	}
	invalid := math.Inf(1)
	if applyControl(&control, ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: 3, Kind: "seek", PositionSeconds: &invalid}, 1003) == nil {
		t.Fatal("accepted nonfinite seek")
	}
	if applyControl(&control, ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: 3, Kind: "loop", LoopEnabled: &enabled}, 1003) == nil {
		t.Fatal("accepted incomplete loop")
	}
}

func TestV6PresenterAndReadinessPolicy(t *testing.T) {
	host, peer := uuid.New(), uuid.New()
	session := model.ProjectSession{CommandSchemaVersion: 6, Mode: model.SessionModeFollowHost, HostMemberID: &host, PluginRequirementsRevision: 2}
	member := model.ProjectSessionMember{ID: host, EffectiveRole: model.ProjectRoleEditor, ReadinessStatus: model.SessionReadinessReady, ReadinessRevision: 2}
	if err := SessionAllowsEdit(session, member); err != nil {
		t.Fatal(err)
	}
	member.ID = peer
	if !errors.Is(SessionAllowsEdit(session, member), ErrForbidden) {
		t.Fatal("non-presenter can edit")
	}
	session.Mode = model.SessionModeSynchronized
	if err := SessionAllowsEdit(session, member); err != nil {
		t.Fatal(err)
	}
	member.ReadinessRevision = 1
	if !errors.Is(SessionAllowsEdit(session, member), ErrPluginNotReady) {
		t.Fatal("stale readiness can edit")
	}
	if NewStore(nil, 99).MaxParticipants != 8 {
		t.Fatal("participant cap exceeded")
	}
}

func TestV6NotebookAndFingerprintContracts(t *testing.T) {
	cues := json.RawMessage(`{"cues":[{"seconds":4.25,"text":"Start"}]}`)
	if err := validateCommandPayloadShapeForSchema("project.setNotebookCues", cues, true, 6); err != nil {
		t.Fatal(err)
	}
	if validateCommandPayloadShapeForSchema("project.setNotebookCues", cues, true, 5) == nil {
		t.Fatal("legacy schema accepted notebook")
	}
	fields, _, err := deriveCommandMetadataForSchema("project.setNotebookCues", cues, true, 6)
	if err != nil || len(fields) != 2 || fields[0] != "project:notebookCues" || fields[1] != "project:renderGeneration" {
		t.Fatalf("wrong notebook fields %v %v", fields, err)
	}
	payload, _ := json.Marshal(map[string]any{"field": "notebookHtml", "value": strings.Repeat("x", 524289)})
	if validateCommandPayloadShapeForSchema("project.setScalar", payload, true, 6) == nil {
		t.Fatal("accepted oversized notebook")
	}
	plug := PluginRequirement{Format: "vst3", NativeUID: "test.plugin", Vendor: "Test", Version: "1", Kind: "effect", ChannelMode: "stereo", ParameterFingerprint: strings.Repeat("a", 64)}
	first, _ := json.Marshal([]PluginRequirement{plug})
	other := plug
	other.ParameterFingerprint = strings.Repeat("b", 64)
	second, _ := json.Marshal([]PluginRequirement{other})
	common, err := commonPluginInventory(model.ProjectSession{PluginPolicy: "external_checked"}, []model.ProjectSessionMember{
		{EffectiveRole: "editor", ReadinessStatus: "ready", PluginInventory: datatypes.JSON(first)},
		{EffectiveRole: "editor", ReadinessStatus: "ready", PluginInventory: datatypes.JSON(second)},
	})
	if err != nil || len(common) != 0 {
		t.Fatalf("incompatible parameter contracts intersect: %v %v", common, err)
	}
}

func TestPostgresV6ModesControlsAndModeration(t *testing.T) {
	dsn := os.Getenv("VLT_COLLAB_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("isolated migrated VLT_COLLAB_TEST_DATABASE_URL required")
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
	now := time.Now().UTC()
	owner := createPublicationActor(t, tx, now, "control-owner")
	editor := createPublicationActor(t, tx, now, "control-editor")
	project := model.CloudProject{ID: uuid.New(), OwnerUserID: owner.user.ID, Title: "Control test", Status: model.ProjectActive, FormatVersion: CollaborationProjectFormatVersion, EngineVersion: "engine-test", MinimumAppVersion: "1.0.0", PluginPolicy: "builtin_only", CreatedAt: now, UpdatedAt: now}
	if err := tx.Create(&project).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Create(&model.ProjectMember{ProjectID: project.ID, UserID: editor.user.ID, Role: "editor", JoinedAt: now, UpdatedAt: now}).Error; err != nil {
		t.Fatal(err)
	}
	store := NewStore(tx)
	store.Now = func() time.Time { return now }
	ctx := context.Background()
	wrongHead, currentHead := int64(1), int64(0)
	engineVersion := "engine-test"
	if _, err := store.UpdateProject(ctx, project.ID, owner.user.ID, UpdateProjectInput{EngineVersion: &engineVersion, ExpectedHeadSeq: &wrongHead}); !errors.Is(err, ErrConflict) {
		t.Fatalf("stale migration proof accepted: %v", err)
	}
	if _, err := store.UpdateProject(ctx, project.ID, editor.user.ID, UpdateProjectInput{EngineVersion: &engineVersion, ExpectedHeadSeq: &currentHead}); !errors.Is(err, ErrForbidden) {
		t.Fatalf("editor migration accepted: %v", err)
	}
	if _, err := store.UpdateProject(ctx, project.ID, owner.user.ID, UpdateProjectInput{EngineVersion: &engineVersion, ExpectedHeadSeq: &currentHead}); err != nil {
		t.Fatalf("verified inactive owner migration rejected: %v", err)
	}
	create := CreateProjectInput{ProjectID: uuid.New(), OwnerUserID: owner.user.ID, Title: "Retry publication", FormatVersion: CollaborationProjectFormatVersion, EngineVersion: "engine-test", MinimumAppVersion: "1.0.0"}
	first, err := store.CreateProject(ctx, create)
	if err != nil {
		t.Fatal(err)
	}
	second, err := store.CreateProject(ctx, create)
	if err != nil || first.Project.ID != second.Project.ID {
		t.Fatalf("create retry duplicated: %v", err)
	}
	create.Title = "Changed request"
	if _, err := store.CreateProject(ctx, create); !errors.Is(err, ErrConflict) {
		t.Fatalf("create reused id accepted changed metadata: %v", err)
	}
	compat := ClientCompatibility{AppVersion: "1.0.0", EngineVersion: "engine-test", CommandSchemaVersion: 6, ProjectFormatVersion: CollaborationProjectFormatVersion, PluginPolicy: "builtin_only"}
	report := PluginReadinessReport{Revision: 1, Plugins: []PluginReadinessResult{}}
	state, err := store.StartSessionV3Secured(ctx, project.ID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeIndependent, compat, SessionSecret{}, nil, report)
	if err != nil {
		t.Fatal(err)
	}
	sessionID := state.Session.ID
	if _, err := store.UpdateProject(ctx, project.ID, owner.user.ID, UpdateProjectInput{EngineVersion: &engineVersion, ExpectedHeadSeq: &currentHead}); !errors.Is(err, ErrConflict) {
		t.Fatalf("live lobby migration accepted: %v", err)
	}
	state, err = store.JoinSessionV3Secured(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, compat, SessionSecret{}, report)
	if err != nil {
		t.Fatal(err)
	}
	var editorMember uuid.UUID
	for _, m := range state.Members {
		if m.UserID == editor.user.ID {
			editorMember = m.ID
		}
	}
	state, err = store.ActivateSession(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID)
	if err != nil {
		t.Fatal(err)
	}
	seed := TransportControl{Rate: 1, PositionSeconds: 9}
	audition := AuditionControl{}
	ownerMarker := EditLeaseInput{FieldKeys: []string{"recording:" + state.Session.HostMemberID.String()}, TTLSeconds: 15, ExpectedSessionVersion: state.Session.Version}
	if _, err := store.ChangeEditLease(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, uuid.Nil, "acquire", ownerMarker); !errors.Is(err, ErrForbidden) {
		t.Fatalf("recording marker identity spoof accepted: %v", err)
	}
	ownerCapture, err := store.ChangeEditLease(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, uuid.Nil, "acquire", ownerMarker)
	if err != nil {
		t.Fatal(err)
	}
	editorCapture, err := store.ChangeEditLease(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, uuid.Nil, "acquire", EditLeaseInput{FieldKeys: []string{"recording:" + editorMember.String()}, TTLSeconds: 15, ExpectedSessionVersion: state.Session.Version})
	if err != nil {
		t.Fatalf("parallel participant recording blocked: %v", err)
	}
	if _, err := store.ChangeSessionMode(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeFollowHost, state.Session.Version, &seed, &audition); !errors.Is(err, ErrRecordingActive) {
		t.Fatalf("mode switch ignored participant recording: %v", err)
	}
	if _, err := store.ChangeEditLease(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, editorCapture.LeaseID, "release", EditLeaseInput{}); err != nil {
		t.Fatal(err)
	}
	if _, err := store.ChangeSessionMode(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeFollowHost, state.Session.Version, &seed, &audition); !errors.Is(err, ErrRecordingActive) {
		t.Fatalf("mode switch ignored remaining recording participant: %v", err)
	}
	if _, err := store.ChangeEditLease(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, ownerCapture.LeaseID, "release", EditLeaseInput{}); err != nil {
		t.Fatal(err)
	}
	recordingLease := model.ProjectTrackLease{ID: uuid.New(), ProjectID: project.ID, SessionID: sessionID, TrackID: uuid.New(), LeaseKind: model.TrackLeaseRecord, HolderMemberID: *state.Session.HostMemberID, AcquiredAt: now, RenewedAt: now, ExpiresAt: now.Add(time.Minute)}
	if err := tx.Create(&recordingLease).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := store.ChangeSessionMode(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeFollowHost, state.Session.Version, &seed, &audition); !errors.Is(err, ErrRecordingActive) {
		t.Fatalf("mode switch revoked active recording: %v", err)
	}
	if err := tx.Delete(&recordingLease).Error; err != nil {
		t.Fatal(err)
	}
	state, err = store.ChangeSessionMode(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeFollowHost, state.Session.Version, &seed, &audition)
	if err != nil {
		t.Fatal(err)
	}
	version := state.Session.Version
	command := AppendOperationInput{ProjectID: project.ID, ActorUserID: editor.user.ID, ActorDeviceID: editor.device.ID, ActorSessionID: editor.session.ID, OpID: uuid.New(), Kind: "project.setScalar", SchemaVersion: 6, SessionVersion: version, Payload: json.RawMessage(`{"field":"name","value":"Renamed"}`), TouchedFields: []string{"project:name", "project:renderGeneration"}}
	if _, _, err := store.AppendOperation(ctx, command); !errors.Is(err, ErrForbidden) {
		t.Fatalf("editor escaped presenter lock: %v", err)
	}
	command.ActorUserID, command.ActorDeviceID, command.ActorSessionID = owner.user.ID, owner.device.ID, owner.session.ID
	command.SessionVersion = version - 1
	if _, _, err := store.AppendOperation(ctx, command); !errors.Is(err, ErrSessionVersion) {
		t.Fatalf("stale policy accepted: %v", err)
	}
	command.SessionVersion = version
	if _, _, err := store.AppendOperation(ctx, command); err != nil {
		t.Fatal(err)
	}
	view, err := store.GetProject(ctx, project.ID, owner.user.ID)
	if err != nil || view.Project.Title != "Renamed" {
		t.Fatalf("name/title diverged: %+v %v", view, err)
	}
	staleRename := command
	staleRename.OpID = uuid.New()
	staleRename.Payload = json.RawMessage(`{"field":"name","value":"Stale metadata"}`)
	if _, _, err := store.AppendOperation(ctx, staleRename); !errors.Is(err, ErrBaseSeqMismatch) {
		t.Fatalf("unguarded rename overwrote newer title: %v", err)
	}
	batchedRename := staleRename
	batchedRename.OpID = uuid.New()
	batchedRename.Kind = "batch"
	batchedRename.Payload = json.RawMessage(`{"commands":[{"kind":"project.setScalar","payload":{"field":"name","value":"Stale batch"},"preconditions":[]}]}`)
	if _, _, err := store.AppendOperation(ctx, batchedRename); !errors.Is(err, ErrBaseSeqMismatch) {
		t.Fatalf("batch escaped title revision guard: %v", err)
	}
	guardedRename := staleRename
	guardedRename.OpID = uuid.New()
	guardedRename.Payload = json.RawMessage(`{"field":"name","value":"Guarded rename"}`)
	guardedRename.Preconditions = []FieldPrecondition{{Kind: "fieldWriterIs", FieldKey: "project:name", OperationID: command.OpID}}
	if _, _, err := store.AppendOperation(ctx, guardedRename); err != nil {
		t.Fatalf("guarded rename cannot rebase: %v", err)
	}
	guardedRename.OpID = uuid.New()
	var titleConflict *PreconditionError
	if _, _, err := store.AppendOperation(ctx, guardedRename); !errors.As(err, &titleConflict) {
		t.Fatalf("stale title writer accepted: %v", err)
	}
	play := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: version, Kind: "play"}
	if _, _, err := store.ApplySessionControl(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, play); !errors.Is(err, ErrForbidden) {
		t.Fatalf("editor controlled presenter: %v", err)
	}
	if _, dup, err := store.ApplySessionControl(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, play); err != nil || dup {
		t.Fatalf("play failed %v %v", dup, err)
	}
	now = now.Add(time.Millisecond)
	stop := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: version, Kind: "stop"}
	if _, _, err := store.ApplySessionControl(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, stop); err != nil {
		t.Fatal(err)
	}
	result, dup, err := store.ApplySessionControl(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, play)
	if err != nil || !dup || result.Control.Transport.Playing {
		t.Fatalf("duplicate play reapplied: %+v %v %v", result, dup, err)
	}
	viewerReport := report
	viewerReport.StayViewer = true
	if _, err := store.UpdatePluginReadiness(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, viewerReport); err != nil {
		t.Fatal(err)
	}
	if _, err := store.HandoffHost(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, editorMember); !errors.Is(err, ErrPluginNotReady) {
		t.Fatalf("blocked host accepted: %v", err)
	}
	if _, err := store.UpdatePluginReadiness(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, report); err != nil {
		t.Fatal(err)
	}
	state, err = store.ChangeSessionMode(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeSynchronized, version, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	play.ActionID = uuid.New()
	play.ExpectedSessionVersion = state.Session.Version
	field := "clip:" + uuid.NewString()
	leaseInput := EditLeaseInput{FieldKeys: []string{field}, TTLSeconds: 15, ExpectedSessionVersion: state.Session.Version}
	lease, err := store.ChangeEditLease(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, uuid.Nil, "acquire", leaseInput)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.ChangeEditLease(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, uuid.Nil, "acquire", EditLeaseInput{FieldKeys: []string{field + ":gain"}}); !errors.Is(err, ErrLeaseHeld) {
		t.Fatalf("overlapping edit lease acquired: %v", err)
	}
	if err := store.enforceEditLeasesTx(tx, state.Session, editorMember, []string{field + ":startSeconds", "project:renderGeneration"}); !errors.Is(err, ErrLeaseHeld) {
		t.Fatalf("journal bypassed edit lease: %v", err)
	}
	if err := store.enforceEditLeasesTx(tx, state.Session, editorMember, []string{"clip:" + uuid.NewString() + ":gain", "project:renderGeneration"}); err != nil {
		t.Fatalf("independent edit blocked: %v", err)
	}
	if _, err := store.ChangeEditLease(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, lease.LeaseID, "release", EditLeaseInput{}); !errors.Is(err, ErrForbidden) {
		t.Fatalf("foreign lease release accepted: %v", err)
	}
	if _, err := store.ChangeEditLease(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, lease.LeaseID, "renew", EditLeaseInput{TTLSeconds: 30}); err != nil {
		t.Fatal(err)
	}
	now = now.Add(31 * time.Second)
	if _, err := store.ChangeEditLease(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, uuid.Nil, "acquire", leaseInput); err != nil {
		t.Fatalf("expired holder blocks editor: %v", err)
	}
	if _, _, err := store.ApplySessionControl(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, play); err != nil {
		t.Fatal(err)
	}
	if _, err := store.ModerateSession(ctx, project.ID, sessionID, owner.user.ID, editor.user.ID, "kick"); err != nil {
		t.Fatal(err)
	}
	if _, err := store.JoinSessionV3Secured(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, compat, SessionSecret{}, report); !errors.Is(err, ErrSessionExcluded) {
		t.Fatalf("kicked user returned: %v", err)
	}
	if _, err := store.ModerateSession(ctx, project.ID, sessionID, owner.user.ID, editor.user.ID, "readmit"); err != nil {
		t.Fatal(err)
	}
	if _, err := store.JoinSessionV3Secured(ctx, project.ID, sessionID, editor.user.ID, editor.device.ID, editor.session.ID, compat, SessionSecret{}, report); err != nil {
		t.Fatal(err)
	}
	if _, err := store.ModerateSession(ctx, project.ID, sessionID, owner.user.ID, editor.user.ID, "ban"); err != nil {
		t.Fatal(err)
	}
	if _, err := store.GetProject(ctx, project.ID, editor.user.ID); !errors.Is(err, ErrProjectBanned) {
		t.Fatalf("ban bypassed: %v", err)
	}
	if _, err := store.ModerateSession(ctx, project.ID, sessionID, owner.user.ID, editor.user.ID, "unban"); err != nil {
		t.Fatal(err)
	}
	if _, err := store.GetProject(ctx, project.ID, editor.user.ID); err != nil {
		t.Fatal(err)
	}
	state, err = store.ChangeSessionMode(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeFollowHost, state.Session.Version, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	assignedHost := *state.Session.HostMemberID
	state, changed, err := store.PauseDisconnectedPresenter(ctx, project.ID, sessionID, assignedHost)
	if err != nil || !changed || state.Session.HostMemberID == nil || *state.Session.HostMemberID != assignedHost || ControlSnapshot(state.Session).Transport.Playing {
		t.Fatalf("presenter loss did not pause: %+v %v", state, err)
	}
	state, err = store.JoinSessionV3Secured(ctx, project.ID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, compat, SessionSecret{}, report)
	if err != nil || state.Session.HostMemberID == nil || *state.Session.HostMemberID != assignedHost || ControlSnapshot(state.Session).Transport.Playing {
		t.Fatalf("returning presenter lost the assigned role or resumed playback: %+v %v", state, err)
	}
}
