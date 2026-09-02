package api

import (
	"context"
	"encoding/json"
	"testing"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
)

func TestPublishReapedSessionMembersDisconnectsAndBroadcastsHostFallback(t *testing.T) {
	bus := collab.NewInProcessRoomBus()
	projectID, sessionID := uuid.New(), uuid.New()
	staleMemberID, nextHostID := uuid.New(), uuid.New()
	stale, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		staleMemberID, 8, collab.DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	survivor, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		nextHostID, 8, collab.DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	server := &Server{Rooms: bus}
	server.publishReapedSessionMembers([]collab.ReapedSessionMembers{{
		ProjectID: projectID, SessionID: sessionID, MemberIDs: []uuid.UUID{staleMemberID},
		PreviousHostMemberID: &staleMemberID, HostMemberID: &nextHostID,
	}})
	select {
	case <-stale.Done():
	case <-time.After(time.Second):
		t.Fatal("stale participant transport was not disconnected")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	for _, wantType := range []string{"presence.leave", "session.host_changed"} {
		message, err := survivor.Next(ctx)
		if err != nil {
			t.Fatalf("receive %s: %v", wantType, err)
		}
		var envelope collaborationServerEnvelope
		if err := json.Unmarshal(message.Data, &envelope); err != nil {
			t.Fatal(err)
		}
		if envelope.Type != wantType {
			t.Fatalf("event type = %q, want %q", envelope.Type, wantType)
		}
		var payload map[string]any
		if err := json.Unmarshal(envelope.Payload, &payload); err != nil {
			t.Fatal(err)
		}
		if payload["reason"] != "heartbeat_timeout" {
			t.Fatalf("event reason = %#v", payload["reason"])
		}
	}
}

func TestSnapshotRequestIsTargetedAndFinalEventDrains(t *testing.T) {
	bus := collab.NewInProcessRoomBus()
	projectID, sessionID := uuid.New(), uuid.New()
	hostID, editorID := uuid.New(), uuid.New()
	host, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		hostID, 8, collab.DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	editor, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		editorID, 8, collab.DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	server := &Server{Rooms: bus, Hashes: collab.NewHashCoordinator(),
		metrics: &collaborationMetrics{}}
	server.publishSnapshotDispatch(collab.SnapshotDispatch{
		RequestID: uuid.New(), ProjectID: projectID, SessionID: sessionID,
		HostMemberID: hostID, TargetSeq: 501, Reason: "session_end",
		Attempt: 2, RetryAtMs: time.Now().Add(time.Second).UnixMilli(),
	})
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	message, err := host.Next(ctx)
	cancel()
	if err != nil {
		t.Fatal(err)
	}
	var envelope collaborationServerEnvelope
	if err := json.Unmarshal(message.Data, &envelope); err != nil {
		t.Fatal(err)
	}
	if envelope.Type != "snapshot.requested" {
		t.Fatalf("targeted event type = %q", envelope.Type)
	}
	if server.metrics.snapshotRequests.Load() != 1 ||
		server.metrics.snapshotRetries.Load() != 1 {
		t.Fatal("snapshot retry metrics were not recorded")
	}
	notTargetContext, cancelNotTarget := context.WithTimeout(context.Background(),
		10*time.Millisecond)
	_, notTargetErr := editor.Next(notTargetContext)
	cancelNotTarget()
	if notTargetErr != context.DeadlineExceeded {
		t.Fatalf("non-host received snapshot request: %v", notTargetErr)
	}

	server.publishSnapshotFinalization(&collab.SnapshotFinalization{
		ProjectID: projectID, SessionID: sessionID, FinalSeq: 501,
	})
	if server.metrics.sessionEnded.Load() != 1 {
		t.Fatal("session finalization metric was not recorded")
	}
	for name, subscription := range map[string]*collab.RoomSubscription{
		"host": host, "editor": editor,
	} {
		readContext, cancelRead := context.WithTimeout(context.Background(), time.Second)
		message, err := subscription.Next(readContext)
		cancelRead()
		if err != nil {
			t.Fatalf("%s terminal event: %v", name, err)
		}
		if err := json.Unmarshal(message.Data, &envelope); err != nil {
			t.Fatal(err)
		}
		if envelope.Type != "session.ended" {
			t.Fatalf("%s terminal type = %q", name, envelope.Type)
		}
		select {
		case <-subscription.Done():
		case <-time.After(time.Second):
			t.Fatalf("%s room did not close after terminal event", name)
		}
	}
}
