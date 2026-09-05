package api

import (
	"context"
	"encoding/json"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
	"vltstudio/backend/internal/model"
)

const collaborationHashTimeout = 15 * time.Second

type openedHashRound struct {
	view     collab.HashRoundView
	expected []uuid.UUID
	protocol string
}

func (s *Server) prepareHashRound(ctx context.Context, projectID uuid.UUID,
	state collab.SessionState) (openedHashRound, error) {
	var unlock func()
	if s.operationSequence != nil {
		unlock = s.operationSequence.lock(projectID)
		defer unlock()
	}
	expected, err := s.connectedEditorParticipants(ctx, projectID, state)
	if err != nil {
		return openedHashRound{}, err
	}
	if len(expected) == 0 {
		s.Hashes.ClearProject(projectID)
		return openedHashRound{}, nil
	}
	var project model.CloudProject
	if err := s.DB.WithContext(ctx).Select("id", "head_seq").
		First(&project, "id = ?", projectID).Error; err != nil {
		return openedHashRound{}, err
	}
	return openedHashRound{
		view: s.Hashes.Begin(projectID, state.Session.ID, project.HeadSeq,
			expected, collaborationHashTimeout),
		expected: expected,
		protocol: func() string {
			protocol, ok := collab.CollaborationProtocolForSchema(
				state.Session.CommandSchemaVersion)
			if !ok {
				return collab.CollaborationProtocolV2
			}
			return protocol
		}(),
	}, nil
}

func (s *Server) publishHashRound(projectID uuid.UUID, round openedHashRound) {
	if round.view.RoundID == uuid.Nil || s.Rooms == nil {
		return
	}
	if s.metrics != nil {
		s.metrics.hashRounds.Add(1)
	}
	payload, _ := json.Marshal(map[string]any{
		"roundId": round.view.RoundID, "sessionId": round.view.SessionID,
		"serverSeq":  round.view.ServerSeq,
		"deadlineMs": round.view.Deadline.UnixMilli(),
	})
	message := collab.RoomMessage{Data: collaborationEnvelopeFor(round.protocol,
		"hash.requested", payload, uuid.Nil, nil, 0)}
	for _, participantID := range round.expected {
		s.Rooms.DeliverParticipant(projectID, participantID, message)
	}
	time.AfterFunc(time.Until(round.view.Deadline), func() {
		missing := s.Hashes.Expire(projectID, round.view.RoundID)
		if len(missing) == 0 {
			return
		}
		if s.metrics != nil {
			s.metrics.hashTimeouts.Add(uint64(len(missing)))
		}
		for _, participantID := range missing {
			s.Rooms.DisconnectParticipant(participantID, collab.RoomClose{
				Code: "hash_timeout", Reason: "state hash response timed out",
			})
		}
		ctx, cancel := context.WithTimeout(context.Background(),
			collaborationWriteTimeout)
		defer cancel()
		if next, err := s.prepareCurrentHashRound(ctx, projectID); err == nil {
			s.publishHashRound(projectID, next)
		} else {
			s.recordCollaborationMaintenanceError("hash-timeout-round", err, false, false)
		}
	})
}

func (s *Server) prepareCurrentHashRound(ctx context.Context,
	projectID uuid.UUID) (openedHashRound, error) {
	var session model.ProjectSession
	if err := s.DB.WithContext(ctx).Where("project_id = ? AND status = ?",
		projectID, model.ProjectSessionActive).First(&session).Error; err != nil {
		s.Hashes.ClearProject(projectID)
		return openedHashRound{}, err
	}
	var members []model.ProjectSessionMember
	if err := s.DB.WithContext(ctx).Where("session_id = ? AND left_at IS NULL",
		session.ID).Order("joined_at, id").Find(&members).Error; err != nil {
		return openedHashRound{}, err
	}
	return s.prepareHashRound(ctx, projectID,
		collab.SessionState{Session: session, Members: members})
}

func (s *Server) connectedEditorParticipants(ctx context.Context,
	projectID uuid.UUID, state collab.SessionState) ([]uuid.UUID, error) {
	connected := make(map[uuid.UUID]bool)
	for _, participantID := range s.Rooms.ConnectedParticipants(projectID) {
		connected[participantID] = true
	}
	var project model.CloudProject
	if err := s.DB.WithContext(ctx).Select("id", "owner_user_id").
		First(&project, "id = ?", projectID).Error; err != nil {
		return nil, err
	}
	var memberships []model.ProjectMember
	if err := s.DB.WithContext(ctx).Where("project_id = ?", projectID).
		Find(&memberships).Error; err != nil {
		return nil, err
	}
	roles := map[uuid.UUID]string{project.OwnerUserID: model.ProjectRoleOwner}
	for _, membership := range memberships {
		roles[membership.UserID] = membership.Role
	}
	result := make([]uuid.UUID, 0, len(connected))
	for _, member := range state.Members {
		role := roles[member.UserID]
		if state.Session.CommandSchemaVersion == collab.CollaborationCommandSchemaV3 {
			role = member.EffectiveRole
		}
		if connected[member.ID] && role != model.ProjectRoleViewer {
			result = append(result, member.ID)
		}
	}
	return result, nil
}
