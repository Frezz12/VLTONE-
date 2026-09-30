package api

import (
	"context"
	"encoding/json"
	"github.com/google/uuid"
	"net/http"
	"time"
	"vltstudio/backend/internal/collab"
)

func (s *Server) publishSessionControl(projectID uuid.UUID, kind string, state collab.SessionState) {
	if s.Rooms == nil || state.Session.CommandSchemaVersion < collab.CollaborationCommandSchemaV6 {
		return
	}
	payload, _ := json.Marshal(map[string]any{"control": collab.ControlSnapshot(state.Session), "hostParticipantId": state.Session.HostMemberID})
	s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: collaborationEnvelopeFor(collab.CollaborationProtocolV6, kind, payload, uuid.Nil, nil, 0)})
}

func (s *Server) publishSessionCatalog(ctx context.Context, projectID, userID uuid.UUID, state collab.SessionState) {
	if s.Rooms == nil || state.Session.CommandSchemaVersion < collab.CollaborationCommandSchemaV6 {
		return
	}
	if catalog, err := s.Collab.SessionPluginCatalog(ctx, projectID, state.Session.ID, userID); err == nil {
		payload, _ := json.Marshal(catalog)
		s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: collaborationEnvelopeFor(collab.CollaborationProtocolV6, "session.catalog_changed", payload, uuid.Nil, nil, 0)})
	}
}

func (s *Server) changeProjectSessionMode(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	var input struct {
		Mode            string                   `json:"mode"`
		ExpectedVersion int64                    `json:"expectedVersion"`
		Transport       *collab.TransportControl `json:"transport"`
		Audition        *collab.AuditionControl  `json:"audition"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	unlock := s.operationSequence.lock(projectID)
	defer unlock()
	state, err := s.Collab.ChangeSessionMode(r.Context(), projectID, sessionID, userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), input.Mode, input.ExpectedVersion, input.Transport, input.Audition)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	s.publishSessionControl(projectID, "session.mode_changed", state)
	writeJSON(w, http.StatusOK, sessionStatePayload(state))
}

func (s *Server) applyProjectSessionControl(ctx context.Context, projectID, sessionID, userID, deviceID, desktopID uuid.UUID, action collab.ControlAction) (collab.ControlResult, bool, error) {
	unlock := s.operationSequence.lock(projectID)
	defer unlock()
	result, duplicate, err := s.Collab.ApplySessionControl(ctx, projectID, sessionID, userID, deviceID, desktopID, action)
	if err == nil && !duplicate && s.Rooms != nil {
		payload, _ := json.Marshal(result)
		s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: collaborationEnvelopeFor(collab.CollaborationProtocolV6, "session.control_changed", payload, uuid.Nil, nil, 0)})
	}
	return result, duplicate, err
}

func (s *Server) projectSessionControl(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	var action collab.ControlAction
	if !decodeJSON(w, r, &action) {
		return
	}
	result, _, err := s.applyProjectSessionControl(r.Context(), projectID, sessionID, userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), action)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, result)
}

func (connection *collaborationRoomConnection) handleSessionControl(ctx context.Context, envelope collaborationClientEnvelope) error {
	if connection.protocol != collab.CollaborationProtocolV6 {
		return collab.ErrVersionMismatch
	}
	var action collab.ControlAction
	if err := decodeCollaborationJSON(envelope.Payload, &action); err != nil {
		return err
	}
	result, duplicate, err := connection.server.applyProjectSessionControl(ctx, connection.projectID, connection.sessionID, connection.userID, connection.deviceID, connection.authSessionID, action)
	if err != nil {
		code, message, retryable := collaborationRejection(err)
		payload, _ := json.Marshal(map[string]any{"requestMessageId": envelope.MessageID, "actionId": action.ActionID, "code": code, "message": message, "retryable": retryable})
		connection.subscription.Deliver(collab.RoomMessage{Data: collaborationEnvelopeFor(connection.protocol, "op.rejected", payload, uuid.Nil, nil, 0)})
		return nil
	}
	if duplicate {
		payload, _ := json.Marshal(result)
		connection.subscription.Deliver(collab.RoomMessage{Data: collaborationEnvelopeFor(connection.protocol, "session.control_changed", payload, uuid.Nil, nil, 0)})
	}
	return nil
}

func (connection *collaborationRoomConnection) handleClockPing(envelope collaborationClientEnvelope) error {
	var input struct {
		ClientSentAtMS int64 `json:"clientSentAtMs"`
	}
	if err := decodeCollaborationJSON(envelope.Payload, &input); err != nil || input.ClientSentAtMS < 0 {
		return collab.ErrValidation
	}
	payload, _ := json.Marshal(map[string]any{"clientSentAtMs": input.ClientSentAtMS, "serverTimeMs": time.Now().UnixMilli()})
	connection.subscription.Deliver(collab.RoomMessage{Data: collaborationEnvelopeFor(connection.protocol, "clock.pong", payload, uuid.Nil, nil, 0)})
	return nil
}

func (s *Server) projectSessionCatalog(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	var result collab.PluginCatalog
	var err error
	if r.Method == http.MethodPut {
		var input struct {
			PluginInventory []collab.PluginRequirement `json:"pluginInventory"`
		}
		if !decodeJSON(w, r, &input) {
			return
		}
		result, err = s.Collab.UpdatePluginInventory(r.Context(), projectID, sessionID, userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), input.PluginInventory)
	} else {
		result, err = s.Collab.SessionPluginCatalog(r.Context(), projectID, sessionID, userFrom(r).ID)
	}
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if r.Method == http.MethodPut && s.Rooms != nil {
		payload, _ := json.Marshal(result)
		s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: collaborationEnvelopeFor(collab.CollaborationProtocolV6, "session.catalog_changed", payload, uuid.Nil, nil, 0)})
	}
	writeJSON(w, http.StatusOK, result)
}

func (s *Server) moderateProjectSession(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	var input struct {
		Action       string    `json:"action"`
		TargetUserID uuid.UUID `json:"targetUserId"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	unlock := s.operationSequence.lock(projectID)
	defer unlock()
	state, err := s.Collab.ModerateSession(r.Context(), projectID, sessionID, userFrom(r).ID, input.TargetUserID, input.Action)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	s.publishSessionCatalog(r.Context(), projectID, userFrom(r).ID, state)
	if s.Rooms != nil {
		if input.Action == "kick" || input.Action == "ban" {
			code := "session_excluded"
			if input.Action == "ban" {
				code = "project_banned"
			}
			s.Rooms.DisconnectProjectUser(projectID, input.TargetUserID, collab.RoomClose{Code: code, Reason: code})
		}
		payload, _ := json.Marshal(map[string]any{"action": input.Action, "targetUserId": input.TargetUserID, "control": collab.ControlSnapshot(state.Session)})
		s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: collaborationEnvelopeFor(collab.CollaborationProtocolV6, "session.participant_moderated", payload, uuid.Nil, nil, 0)})
	}
	writeJSON(w, http.StatusOK, sessionStatePayload(state))
}
