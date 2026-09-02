package collab

import (
	"errors"
	"sync"
	"time"

	"github.com/google/uuid"
)

var ErrHashConsensusBlocked = errors.New("project writes are paused for hash consensus")

type HashDecision string

const (
	HashNoChange HashDecision = "no_change"
	HashVerified HashDecision = "verified"
	HashResync   HashDecision = "resync"
	HashConflict HashDecision = "conflict"
)

type HashRoundView struct {
	RoundID   uuid.UUID
	SessionID uuid.UUID
	ServerSeq int64
	Deadline  time.Time
}

type HashResult struct {
	Decision HashDecision
	Round    HashRoundView
}

type hashRound struct {
	HashRoundView
	expectedHash  string
	expected      map[uuid.UUID]bool
	reports       map[uuid.UUID]string
	blocked       bool
	mismatchCount int
}

// HashCoordinator is deliberately in-memory: collaboration v2 is restricted
// to one API process. Every live project must complete a server-opened round
// before writes are admitted.
type HashCoordinator struct {
	mu     sync.RWMutex
	rounds map[uuid.UUID]*hashRound
}

func NewHashCoordinator() *HashCoordinator {
	return &HashCoordinator{rounds: make(map[uuid.UUID]*hashRound)}
}

func (coordinator *HashCoordinator) Begin(projectID, sessionID uuid.UUID,
	serverSeq int64, expectedParticipants []uuid.UUID, timeout time.Duration) HashRoundView {
	if projectID == uuid.Nil || sessionID == uuid.Nil || serverSeq < 0 {
		return HashRoundView{}
	}
	if timeout <= 0 {
		timeout = 15 * time.Second
	}
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	mismatches := 0
	if current := coordinator.rounds[projectID]; current != nil &&
		current.SessionID == sessionID && current.blocked {
		mismatches = current.mismatchCount
	}
	round := newHashRound(sessionID, serverSeq, expectedParticipants,
		mismatches, time.Now().UTC().Add(timeout))
	coordinator.rounds[projectID] = round
	return round.HashRoundView
}

func (coordinator *HashCoordinator) Current(projectID uuid.UUID) (HashRoundView, bool) {
	coordinator.mu.RLock()
	defer coordinator.mu.RUnlock()
	round := coordinator.rounds[projectID]
	if round == nil {
		return HashRoundView{}, false
	}
	return round.HashRoundView, true
}

// AcquireAppendPermit holds a read lock through the durable commit and room
// publication. Absence of a verified round is fail-closed after server restart.
func (coordinator *HashCoordinator) AcquireAppendPermit(
	projectID uuid.UUID) (release func(), err error) {
	coordinator.mu.RLock()
	round := coordinator.rounds[projectID]
	if round == nil || round.blocked {
		coordinator.mu.RUnlock()
		return nil, ErrHashConsensusBlocked
	}
	return coordinator.mu.RUnlock, nil
}

func (coordinator *HashCoordinator) Report(projectID, roundID, sessionID,
	reporterID uuid.UUID, serverSeq int64, sha256 string) HashResult {
	if projectID == uuid.Nil || roundID == uuid.Nil || sessionID == uuid.Nil ||
		reporterID == uuid.Nil || serverSeq < 0 || len(sha256) != 64 {
		return HashResult{Decision: HashNoChange}
	}
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	round := coordinator.rounds[projectID]
	if round == nil || round.RoundID != roundID || round.SessionID != sessionID ||
		round.ServerSeq != serverSeq || !round.expected[reporterID] || !round.blocked {
		return HashResult{Decision: HashNoChange}
	}
	round.reports[reporterID] = sha256
	if round.expectedHash == "" {
		round.expectedHash = sha256
	}
	if sha256 != round.expectedHash {
		round.mismatchCount++
		if round.mismatchCount >= 2 {
			return HashResult{Decision: HashConflict, Round: round.HashRoundView}
		}
		next := newHashRound(round.SessionID, round.ServerSeq,
			participantKeys(round.expected), round.mismatchCount,
			time.Now().UTC().Add(15*time.Second))
		coordinator.rounds[projectID] = next
		return HashResult{Decision: HashResync, Round: next.HashRoundView}
	}
	for participantID := range round.expected {
		if round.reports[participantID] != round.expectedHash {
			return HashResult{Decision: HashNoChange, Round: round.HashRoundView}
		}
	}
	round.blocked = false
	round.mismatchCount = 0
	return HashResult{Decision: HashVerified, Round: round.HashRoundView}
}

// Expire returns only participants that failed to report for this exact epoch.
// The caller disconnects them and opens a fresh round for remaining editors.
func (coordinator *HashCoordinator) Expire(projectID, roundID uuid.UUID) []uuid.UUID {
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	round := coordinator.rounds[projectID]
	if round == nil || round.RoundID != roundID || !round.blocked ||
		time.Now().UTC().Before(round.Deadline) {
		return nil
	}
	missing := make([]uuid.UUID, 0, len(round.expected))
	for participantID := range round.expected {
		if _, reported := round.reports[participantID]; !reported {
			missing = append(missing, participantID)
		}
	}
	delete(coordinator.rounds, projectID)
	return missing
}

func (coordinator *HashCoordinator) ClearProject(projectID uuid.UUID) {
	coordinator.mu.Lock()
	delete(coordinator.rounds, projectID)
	coordinator.mu.Unlock()
}

func newHashRound(sessionID uuid.UUID, serverSeq int64,
	participants []uuid.UUID, mismatchCount int, deadline time.Time) *hashRound {
	expected := make(map[uuid.UUID]bool, len(participants))
	for _, participantID := range participants {
		if participantID != uuid.Nil {
			expected[participantID] = true
		}
	}
	return &hashRound{
		HashRoundView: HashRoundView{RoundID: uuid.New(), SessionID: sessionID,
			ServerSeq: serverSeq, Deadline: deadline},
		expected: expected, reports: make(map[uuid.UUID]string), blocked: true,
		mismatchCount: mismatchCount,
	}
}

func participantKeys(values map[uuid.UUID]bool) []uuid.UUID {
	result := make([]uuid.UUID, 0, len(values))
	for value := range values {
		result = append(result, value)
	}
	return result
}
