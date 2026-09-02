package collab

import (
	"errors"
	"sync"

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

type hashRound struct {
	sessionID     uuid.UUID
	serverSeq     int64
	hostID        uuid.UUID
	expectedHash  string
	expected      map[uuid.UUID]bool
	reports       map[uuid.UUID]string
	blocked       bool
	roundFailed   bool
	mismatchCount int
}

// HashCoordinator is intentionally in-memory in v1, matching the single Go
// API instance. It guards operation appends while clients replay after a hash
// mismatch. Its API does not depend on WebSockets so a distributed coordinator
// can replace it together with RoomBus later.
type HashCoordinator struct {
	mu     sync.RWMutex
	rounds map[uuid.UUID]*hashRound
}

func NewHashCoordinator() *HashCoordinator {
	return &HashCoordinator{rounds: make(map[uuid.UUID]*hashRound)}
}

// AcquireAppendPermit holds a read lock until release, so a mismatch cannot be
// declared concurrently between the guard check and the durable DB commit.
func (coordinator *HashCoordinator) AcquireAppendPermit(
	projectID uuid.UUID) (release func(), err error) {
	coordinator.mu.RLock()
	round := coordinator.rounds[projectID]
	if round != nil && round.blocked {
		coordinator.mu.RUnlock()
		return nil, ErrHashConsensusBlocked
	}
	return coordinator.mu.RUnlock, nil
}

func (coordinator *HashCoordinator) Report(projectID, sessionID, reporterID,
	hostID uuid.UUID, serverSeq int64, sha256 string,
	expectedParticipants []uuid.UUID) HashDecision {
	if projectID == uuid.Nil || sessionID == uuid.Nil || reporterID == uuid.Nil ||
		hostID == uuid.Nil || serverSeq < 0 || len(sha256) != 64 {
		return HashNoChange
	}
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	round := coordinator.rounds[projectID]
	if round == nil || round.sessionID != sessionID || round.serverSeq != serverSeq {
		if reporterID != hostID {
			return HashNoChange
		}
		round = newHashRound(sessionID, serverSeq, hostID, sha256,
			expectedParticipants, 0)
		coordinator.rounds[projectID] = round
	} else if reporterID == hostID && round.blocked {
		// The host starts the one allowed recovery replay round. A second
		// mismatch before a verified quorum escalates to conflict/read-only.
		round = newHashRound(sessionID, serverSeq, hostID, sha256,
			expectedParticipants, round.mismatchCount)
		round.blocked = true
		coordinator.rounds[projectID] = round
	} else if reporterID == hostID && round.expectedHash != sha256 {
		// A new host canonical state at the same sequence is a fresh round.
		round = newHashRound(sessionID, serverSeq, hostID, sha256,
			expectedParticipants, round.mismatchCount)
		coordinator.rounds[projectID] = round
	}
	if round.hostID != hostID {
		if reporterID != hostID {
			return HashNoChange
		}
		round.hostID = hostID
		round.expectedHash = sha256
	}
	if !round.expected[reporterID] {
		return HashNoChange
	}
	round.reports[reporterID] = sha256
	if sha256 != round.expectedHash {
		if round.roundFailed {
			if round.mismatchCount >= 2 {
				return HashConflict
			}
			return HashResync
		}
		round.roundFailed = true
		round.blocked = true
		round.mismatchCount++
		if round.mismatchCount >= 2 {
			return HashConflict
		}
		return HashResync
	}
	for participantID := range round.expected {
		if round.reports[participantID] != round.expectedHash {
			return HashNoChange
		}
	}
	round.blocked = false
	round.roundFailed = false
	round.mismatchCount = 0
	return HashVerified
}

func (coordinator *HashCoordinator) ClearProject(projectID uuid.UUID) {
	coordinator.mu.Lock()
	delete(coordinator.rounds, projectID)
	coordinator.mu.Unlock()
}

func newHashRound(sessionID uuid.UUID, serverSeq int64, hostID uuid.UUID,
	sha256 string, participants []uuid.UUID, mismatchCount int) *hashRound {
	expected := make(map[uuid.UUID]bool, len(participants)+1)
	expected[hostID] = true
	for _, participantID := range participants {
		if participantID != uuid.Nil {
			expected[participantID] = true
		}
	}
	return &hashRound{
		sessionID: sessionID, serverSeq: serverSeq, hostID: hostID,
		expectedHash: sha256, expected: expected,
		reports: make(map[uuid.UUID]string), mismatchCount: mismatchCount,
	}
}
