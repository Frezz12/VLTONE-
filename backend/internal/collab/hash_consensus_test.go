package collab

import (
	"errors"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestHashConsensusBlocksUntilServerOpenedRoundIsVerified(t *testing.T) {
	coordinator := NewHashCoordinator()
	projectID, sessionID := uuid.New(), uuid.New()
	hostID, editorID := uuid.New(), uuid.New()
	good := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	round := coordinator.Begin(projectID, sessionID, 12,
		[]uuid.UUID{hostID, editorID}, time.Minute)
	if _, err := coordinator.AcquireAppendPermit(projectID); !errors.Is(err, ErrHashConsensusBlocked) {
		t.Fatalf("unverified round admitted append: %v", err)
	}
	if result := coordinator.Report(projectID, round.RoundID, sessionID,
		hostID, 12, good); result.Decision != HashNoChange {
		t.Fatalf("first report decision = %q", result.Decision)
	}
	if result := coordinator.Report(projectID, round.RoundID, sessionID,
		editorID, 12, good); result.Decision != HashVerified {
		t.Fatalf("verified decision = %q", result.Decision)
	}
	release, err := coordinator.AcquireAppendPermit(projectID)
	if err != nil {
		t.Fatalf("verified round kept writes blocked: %v", err)
	}
	release()
}

func TestHashConsensusEscalatesSecondMismatch(t *testing.T) {
	coordinator := NewHashCoordinator()
	projectID, sessionID := uuid.New(), uuid.New()
	hostID, editorID := uuid.New(), uuid.New()
	good := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	bad := "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	round := coordinator.Begin(projectID, sessionID, 3,
		[]uuid.UUID{hostID, editorID}, time.Minute)
	coordinator.Report(projectID, round.RoundID, sessionID, hostID, 3, good)
	result := coordinator.Report(projectID, round.RoundID, sessionID, editorID, 3, bad)
	if result.Decision != HashResync || result.Round.RoundID == round.RoundID {
		t.Fatalf("first mismatch result = %#v", result)
	}
	coordinator.Report(projectID, result.Round.RoundID, sessionID, hostID, 3, good)
	result = coordinator.Report(projectID, result.Round.RoundID, sessionID, editorID, 3, bad)
	if result.Decision != HashConflict {
		t.Fatalf("second mismatch decision = %q", result.Decision)
	}
	if _, err := coordinator.AcquireAppendPermit(projectID); !errors.Is(err, ErrHashConsensusBlocked) {
		t.Fatalf("conflicted round admitted append: %v", err)
	}
}

func TestHashConsensusFailsClosedWithoutRound(t *testing.T) {
	coordinator := NewHashCoordinator()
	if _, err := coordinator.AcquireAppendPermit(uuid.New()); !errors.Is(err, ErrHashConsensusBlocked) {
		t.Fatalf("missing round admitted append: %v", err)
	}
}
