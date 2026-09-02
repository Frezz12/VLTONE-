package collab

import (
	"errors"
	"testing"

	"github.com/google/uuid"
)

func TestHashConsensusBlocksResyncAndEscalatesRepeatedMismatch(t *testing.T) {
	coordinator := NewHashCoordinator()
	projectID, sessionID := uuid.New(), uuid.New()
	hostID, editorID := uuid.New(), uuid.New()
	good := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	bad := "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	participants := []uuid.UUID{hostID, editorID}
	if decision := coordinator.Report(projectID, sessionID, hostID, hostID,
		12, good, participants); decision != HashNoChange {
		t.Fatalf("host-only decision = %q", decision)
	}
	if decision := coordinator.Report(projectID, sessionID, editorID, hostID,
		12, bad, participants); decision != HashResync {
		t.Fatalf("first mismatch decision = %q", decision)
	}
	if _, err := coordinator.AcquireAppendPermit(projectID); !errors.Is(err, ErrHashConsensusBlocked) {
		t.Fatalf("append was not blocked: %v", err)
	}
	if decision := coordinator.Report(projectID, sessionID, hostID, hostID,
		12, good, participants); decision != HashNoChange {
		t.Fatalf("recovery host decision = %q", decision)
	}
	if decision := coordinator.Report(projectID, sessionID, editorID, hostID,
		12, bad, participants); decision != HashConflict {
		t.Fatalf("second mismatch decision = %q", decision)
	}
}

func TestHashConsensusVerifiedReplayReopensWrites(t *testing.T) {
	coordinator := NewHashCoordinator()
	projectID, sessionID := uuid.New(), uuid.New()
	hostID, editorID := uuid.New(), uuid.New()
	good := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	bad := "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	participants := []uuid.UUID{hostID, editorID}
	coordinator.Report(projectID, sessionID, hostID, hostID, 3, good, participants)
	coordinator.Report(projectID, sessionID, editorID, hostID, 3, bad, participants)
	coordinator.Report(projectID, sessionID, hostID, hostID, 3, good, participants)
	if decision := coordinator.Report(projectID, sessionID, editorID, hostID,
		3, good, participants); decision != HashVerified {
		t.Fatalf("verified replay decision = %q", decision)
	}
	release, err := coordinator.AcquireAppendPermit(projectID)
	if err != nil {
		t.Fatalf("writes stayed blocked: %v", err)
	}
	release()
}
