package api

import (
	"context"
	"sync"

	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
	"vltstudio/backend/internal/model"
)

type projectAppendLock struct {
	mutex sync.Mutex
	refs  int
}

type operationSequencer struct {
	mu    sync.Mutex
	locks map[uuid.UUID]*projectAppendLock
}

func newOperationSequencer() *operationSequencer {
	return &operationSequencer{locks: make(map[uuid.UUID]*projectAppendLock)}
}

func (sequencer *operationSequencer) lock(projectID uuid.UUID) func() {
	sequencer.mu.Lock()
	entry := sequencer.locks[projectID]
	if entry == nil {
		entry = &projectAppendLock{}
		sequencer.locks[projectID] = entry
	}
	entry.refs++
	sequencer.mu.Unlock()
	entry.mutex.Lock()
	return func() {
		entry.mutex.Unlock()
		sequencer.mu.Lock()
		entry.refs--
		if entry.refs == 0 {
			delete(sequencer.locks, projectID)
		}
		sequencer.mu.Unlock()
	}
}

// appendCollaborationOperation is the only API path that commits commands.
// Holding the project lock through Publish preserves database sequence order
// for both REST and WebSocket callers.
func (s *Server) appendCollaborationOperation(ctx context.Context,
	input collab.AppendOperationInput) (model.ProjectOperation, bool, error) {
	if !cloudRecordingEnabledV1 && input.Kind == "recording.commit" {
		if s.metrics != nil {
			s.metrics.rejections.Add(1)
		}
		return model.ProjectOperation{}, false, collab.ErrCloudRecordingDisabled
	}
	unlock := s.operationSequence.lock(input.ProjectID)
	defer unlock()
	releasePermit, err := s.Hashes.AcquireAppendPermit(input.ProjectID)
	if err != nil {
		if s.metrics != nil {
			s.metrics.rejections.Add(1)
		}
		return model.ProjectOperation{}, false, err
	}
	defer releasePermit()
	operation, duplicate, err := s.Collab.AppendOperation(ctx, input)
	if err != nil {
		if s.metrics != nil {
			s.metrics.rejections.Add(1)
		}
		return model.ProjectOperation{}, false, err
	}
	if !duplicate && s.metrics != nil {
		s.metrics.operations.Add(1)
	}
	if !duplicate && s.Rooms != nil {
		s.Rooms.Publish(input.ProjectID, uuid.Nil, collab.RoomMessage{
			Data: collaborationCommittedEnvelope(operation),
		})
	}
	return operation, duplicate, nil
}
