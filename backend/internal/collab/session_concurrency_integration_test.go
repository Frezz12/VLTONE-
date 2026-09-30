package collab

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/google/uuid"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
)

// Uses committed uniquely identified fixtures so each goroutine gets a real
// independent PostgreSQL transaction. Only these fixtures are removed afterward.
func TestPostgresV6EightConcurrentParticipants(t *testing.T) {
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
	now := time.Now().UTC()
	actors := make([]publicationActor, 9)
	projectID := uuid.New()
	for i := range actors {
		actors[i] = createPublicationActor(t, db, now, fmt.Sprintf("eight-client-%d", i))
	}
	t.Cleanup(func() {
		if err := db.Delete(&model.CloudProject{}, "id = ?", projectID).Error; err != nil {
			t.Error(err)
		}
		for _, actor := range actors {
			for _, entry := range []struct {
				value any
				id    uuid.UUID
			}{{&model.DesktopSession{}, actor.session.ID}, {&model.Device{}, actor.device.ID}, {&model.User{}, actor.user.ID}} {
				if err := db.Delete(entry.value, "id = ?", entry.id).Error; err != nil {
					t.Error(err)
				}
			}
		}
	})
	project := model.CloudProject{ID: projectID, OwnerUserID: actors[0].user.ID, Title: "Eight clients", Status: model.ProjectActive, FormatVersion: CollaborationProjectFormatVersion, EngineVersion: "engine-test", MinimumAppVersion: "1.0.0", PluginPolicy: "builtin_only", CreatedAt: now, UpdatedAt: now}
	if err := db.Create(&project).Error; err != nil {
		t.Fatal(err)
	}
	for _, actor := range actors[1:] {
		if err := db.Create(&model.ProjectMember{ProjectID: projectID, UserID: actor.user.ID, Role: "editor", JoinedAt: now, UpdatedAt: now}).Error; err != nil {
			t.Fatal(err)
		}
	}
	store := NewStore(db)
	ctx := context.Background()
	compat := ClientCompatibility{AppVersion: "1.0.0", EngineVersion: "engine-test", CommandSchemaVersion: 6, ProjectFormatVersion: CollaborationProjectFormatVersion, PluginPolicy: "builtin_only"}
	report := PluginReadinessReport{Revision: 1, Plugins: []PluginReadinessResult{}}
	owner := actors[0]
	state, err := store.StartSessionV3Secured(ctx, projectID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeIndependent, compat, SessionSecret{}, nil, report)
	if err != nil {
		t.Fatal(err)
	}
	sessionID := state.Session.ID
	for _, actor := range actors[1:8] {
		state, err = store.JoinSessionV3Secured(ctx, projectID, sessionID, actor.user.ID, actor.device.ID, actor.session.ID, compat, SessionSecret{}, report)
		if err != nil {
			t.Fatal(err)
		}
	}
	ninth := actors[8]
	if _, err := store.JoinSessionV3Secured(ctx, projectID, sessionID, ninth.user.ID, ninth.device.ID, ninth.session.ID, compat, SessionSecret{}, report); !errors.Is(err, ErrSessionFull) {
		t.Fatalf("ninth participant accepted: %v", err)
	}
	mismatch := compat
	mismatch.AppVersion = "1.0.1"
	if _, err := store.JoinSessionV3Secured(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, mismatch, SessionSecret{}, report); !errors.Is(err, ErrVersionMismatch) {
		t.Fatalf("mixed application version accepted: %v", err)
	}
	state, err = store.ActivateSession(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID)
	if err != nil {
		t.Fatal(err)
	}
	state, err = store.ChangeSessionMode(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeSynchronized, state.Session.Version, &TransportControl{Rate: 1}, &AuditionControl{})
	if err != nil {
		t.Fatal(err)
	}
	epoch := state.Session.Version
	failures := make(chan error, 256)
	var workers sync.WaitGroup
	for index, actor := range actors[:8] {
		workers.Add(1)
		go func(index int, actor publicationActor) {
			defer workers.Done()
			for i := 0; i < 8; i++ {
				payload, _ := json.Marshal(map[string]any{"field": "masterPan", "value": float64(index) / 8})
				command := AppendOperationInput{ProjectID: projectID, ActorUserID: actor.user.ID, ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID, OpID: uuid.New(), Kind: "project.setScalar", SchemaVersion: 6, SessionVersion: epoch, Payload: payload, TouchedFields: []string{"project:masterPan", "project:renderGeneration"}}
				if _, _, err := store.AppendOperation(ctx, command); err != nil {
					failures <- err
				}
				position := float64(index*8 + i)
				action := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: epoch, Kind: "play", PositionSeconds: &position}
				if _, _, err := store.ApplySessionControl(ctx, projectID, sessionID, actor.user.ID, actor.device.ID, actor.session.ID, action); err != nil {
					failures <- err
				}
				if _, duplicate, err := store.ApplySessionControl(ctx, projectID, sessionID, actor.user.ID, actor.device.ID, actor.session.ID, action); err != nil || !duplicate {
					failures <- fmt.Errorf("control receipt lost: %v", err)
				}
			}
		}(index, actor)
	}
	workers.Wait()
	close(failures)
	for failure := range failures {
		t.Error(failure)
	}
	if t.Failed() {
		return
	}
	var ops []model.ProjectOperation
	if err := db.Where("project_id = ?", projectID).Order("seq").Find(&ops).Error; err != nil {
		t.Fatal(err)
	}
	if len(ops) != 64 {
		t.Fatalf("lost concurrent edits: %d", len(ops))
	}
	for index, op := range ops {
		if op.Seq != int64(index+1) {
			t.Fatal("operation sequence has a gap")
		}
	}
	// Every participant can have old-policy work in flight while the owner
	// switches to presenter mode. The project row serializes both operations:
	// a command commits before the switch or rejects without changing the log.
	start := make(chan struct{})
	raceFailures := make(chan error, 128)
	var acceptedDuringSwitch atomic.Int64
	for index, actor := range actors[:8] {
		workers.Add(1)
		go func(index int, actor publicationActor) {
			defer workers.Done()
			<-start
			for i := 0; i < 8; i++ {
				payload, _ := json.Marshal(map[string]any{"field": "masterPan", "value": float64(index) / 8})
				command := AppendOperationInput{ProjectID: projectID, ActorUserID: actor.user.ID, ActorDeviceID: actor.device.ID, ActorSessionID: actor.session.ID, OpID: uuid.New(), Kind: "project.setScalar", SchemaVersion: 6, SessionVersion: epoch, Payload: payload, TouchedFields: []string{"project:masterPan", "project:renderGeneration"}}
				if _, _, err := store.AppendOperation(ctx, command); err == nil {
					acceptedDuringSwitch.Add(1)
				} else if !errors.Is(err, ErrSessionVersion) && !errors.Is(err, ErrForbidden) {
					raceFailures <- err
				}
				action := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: epoch, Kind: "stop"}
				if _, _, err := store.ApplySessionControl(ctx, projectID, sessionID, actor.user.ID, actor.device.ID, actor.session.ID, action); err != nil && !errors.Is(err, ErrSessionVersion) && !errors.Is(err, ErrForbidden) {
					raceFailures <- err
				}
			}
		}(index, actor)
	}
	close(start)
	state, err = store.ChangeSessionMode(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, model.SessionModeFollowHost, epoch, nil, nil)
	workers.Wait()
	close(raceFailures)
	for failure := range raceFailures {
		t.Error(failure)
	}
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Where("project_id = ?", projectID).Order("seq").Find(&ops).Error; err != nil {
		t.Fatal(err)
	}
	if int64(len(ops)) != 64+acceptedDuringSwitch.Load() {
		t.Fatal("mode switch partially applied or lost an acknowledged operation")
	}
	for index, op := range ops {
		if op.Seq != int64(index+1) {
			t.Fatal("mode switch introduced an operation sequence gap")
		}
	}
	stale := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: epoch, Kind: "stop"}
	if _, _, err := store.ApplySessionControl(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, stale); !errors.Is(err, ErrSessionVersion) {
		t.Fatalf("old mode control applied: %v", err)
	}
	stop := ControlAction{ActionID: uuid.New(), ExpectedSessionVersion: state.Session.Version, Kind: "stop"}
	result, _, err := store.ApplySessionControl(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, stop)
	if err != nil || result.Control.Transport.Revision != ControlSnapshot(state.Session).Transport.Revision+1 {
		t.Fatalf("reliable control count mismatch: %v %+v", err, result.Control)
	}
	rejoined, err := store.JoinSessionV3Secured(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, compat, SessionSecret{}, report)
	if err != nil {
		t.Fatal(err)
	}
	restored := ControlSnapshot(rejoined.Session)
	if restored.Transport != result.Control.Transport || restored.SessionVersion != state.Session.Version {
		t.Fatal("reconnect lost authoritative control")
	}
	assignedHost := *state.Session.HostMemberID
	lost, changed, err := store.PauseDisconnectedPresenter(ctx, projectID, sessionID, assignedHost)
	if err != nil || !changed || lost.Session.HostMemberID == nil || *lost.Session.HostMemberID != assignedHost || ControlSnapshot(lost.Session).Transport.Playing {
		t.Fatalf("lost presenter assignment was changed: %v", err)
	}
	rejoined, err = store.JoinSessionV3Secured(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, compat, SessionSecret{}, report)
	if err != nil || rejoined.Session.HostMemberID == nil || *rejoined.Session.HostMemberID != assignedHost {
		t.Fatalf("reconnect lost the assigned presenter: %v", err)
	}
	// A longer outage replaces the membership, not the user's assigned role.
	if err := db.Model(&model.ProjectSessionMember{}).Where("id = ?", assignedHost).
		Update("last_seen_at", time.Now().UTC().Add(-2*time.Minute)).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := store.ReapStaleSessionMembers(ctx, time.Minute, 100); err != nil {
		t.Fatal(err)
	}
	rejoined, err = store.JoinSessionV3Secured(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, compat, SessionSecret{}, report)
	if err != nil || rejoined.Session.HostMemberID == nil || *rejoined.Session.HostMemberID == assignedHost || ControlSnapshot(rejoined.Session).Transport.Playing {
		t.Fatalf("presenter did not recover after heartbeat expiry: %+v %v", rejoined, err)
	}
	newHost := *rejoined.Session.HostMemberID
	var replacement uuid.UUID
	for _, member := range rejoined.Members {
		if member.ID == newHost && SessionAllowsEdit(rejoined.Session, member) != nil {
			t.Fatal("returning presenter cannot edit")
		}
		if member.UserID == actors[1].user.ID {
			replacement = member.ID
		}
	}
	rejoined, err = store.HandoffHost(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, replacement)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.HandoffHost(ctx, projectID, sessionID, actors[1].user.ID, actors[1].device.ID, actors[1].session.ID, newHost); !errors.Is(err, ErrForbidden) {
		t.Fatalf("presenter inherited the owner's assignment permission: %v", err)
	}
	rejoined, err = store.JoinSessionV3Secured(ctx, projectID, sessionID, owner.user.ID, owner.device.ID, owner.session.ID, compat, SessionSecret{}, report)
	if err != nil || rejoined.Session.HostMemberID == nil || *rejoined.Session.HostMemberID != replacement {
		t.Fatalf("returning former presenter replaced the owner's selection: %v", err)
	}
	if err := db.Model(&model.ProjectSessionMember{}).Where("id = ?", replacement).
		Update("last_seen_at", time.Now().UTC().Add(-2*time.Minute)).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := store.ReapStaleSessionMembers(ctx, time.Minute, 100); err != nil {
		t.Fatal(err)
	}
	removed, err := store.ModerateSession(ctx, projectID, sessionID, owner.user.ID, actors[1].user.ID, "kick")
	if err != nil || removed.Session.HostMemberID != nil {
		t.Fatalf("offline presenter retained role after exclusion: %v", err)
	}
	if _, err := store.ModerateSession(ctx, projectID, sessionID, owner.user.ID, actors[1].user.ID, "readmit"); err != nil {
		t.Fatal(err)
	}
	rejoined, err = store.JoinSessionV3Secured(ctx, projectID, sessionID, actors[1].user.ID, actors[1].device.ID, actors[1].session.ID, compat, SessionSecret{}, report)
	if err != nil || rejoined.Session.HostMemberID != nil {
		t.Fatalf("readmitted participant reclaimed a revoked assignment: %v", err)
	}
}
