package api

import (
	"errors"
	"testing"

	"github.com/coder/websocket"
	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
)

func TestLateCollaborationSubscriptionRequiresCurrentActorState(t *testing.T) {
	valid := collaborationLiveActorState{
		globalEnabled: true, userActive: true, entitled: true,
		deviceActive: true, desktopSessionActive: true,
		projectRole: "editor", participantActive: true,
	}
	tests := []struct {
		name         string
		state        collaborationLiveActorState
		expectedRole string
		err          error
		code         string
	}{
		{name: "collaboration disabled", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.globalEnabled = false }),
			expectedRole: "editor", code: "collaboration_access_disabled"},
		{name: "entitlement revoked", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.entitled = false }),
			expectedRole: "editor", code: "collaboration_access_disabled"},
		{name: "user suspended", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.userActive = false }),
			expectedRole: "editor", code: "account_suspended"},
		{name: "device revoked", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.deviceActive = false }),
			expectedRole: "editor", code: "device_revoked"},
		{name: "desktop session revoked or expired", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.desktopSessionActive = false }),
			expectedRole: "editor", code: "desktop_session_revoked"},
		{name: "project member removed", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.participantActive = false }),
			expectedRole: "editor", code: "member_removed"},
		{name: "project role removed", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.projectRole = "" }),
			expectedRole: "editor", code: "member_removed"},
		{name: "project role changed", state: withLiveActorState(valid,
			func(state *collaborationLiveActorState) { state.projectRole = "viewer" }),
			expectedRole: "editor", code: "role_changed"},
		{name: "database unavailable", state: valid, expectedRole: "editor",
			err:  errors.New("database unavailable"),
			code: "collaboration_access_unavailable"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			bus := collab.NewInProcessRoomBus()
			subscription, err := bus.Subscribe(uuid.New(), uuid.New(), uuid.New(),
				uuid.New(), uuid.New(), 2, collab.DefaultRoomQueueBytes)
			if err != nil {
				t.Fatal(err)
			}
			closeInfo, allowed := enforceCollaborationSubscriptionAccess(
				subscription, test.state, test.expectedRole, test.err)
			if allowed || closeInfo.Code != test.code ||
				subscription.CloseInfo().Code != test.code {
				t.Fatalf("late actor result = allowed:%v close:%#v stored:%#v",
					allowed, closeInfo, subscription.CloseInfo())
			}
			select {
			case <-subscription.Done():
			default:
				t.Fatal("late subscription remained registered")
			}
			if test.err == nil {
				if status := collaborationWebSocketCloseStatus(closeInfo, nil); status != websocket.StatusPolicyViolation {
					t.Fatalf("revocation close status = %v", status)
				}
			}
		})
	}
}

func TestLateCollaborationSubscriptionAllowsCurrentActor(t *testing.T) {
	bus := collab.NewInProcessRoomBus()
	subscription, err := bus.Subscribe(uuid.New(), uuid.New(), uuid.New(),
		uuid.New(), uuid.New(), 2, collab.DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	state := collaborationLiveActorState{
		globalEnabled: true, userActive: true, entitled: true,
		deviceActive: true, desktopSessionActive: true,
		projectRole: "viewer", participantActive: true,
	}
	closeInfo, allowed := enforceCollaborationSubscriptionAccess(subscription,
		state, "viewer", nil)
	if !allowed || closeInfo.Code != "" {
		t.Fatalf("valid actor was rejected: allowed=%v close=%#v",
			allowed, closeInfo)
	}
	select {
	case <-subscription.Done():
		t.Fatal("valid actor subscription was closed")
	default:
	}
	subscription.Close(collab.RoomClose{Code: "test_complete"})
}

func withLiveActorState(state collaborationLiveActorState,
	change func(*collaborationLiveActorState)) collaborationLiveActorState {
	change(&state)
	return state
}
