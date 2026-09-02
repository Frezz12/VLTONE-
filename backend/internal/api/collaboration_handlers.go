package api

import (
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5/middleware"
	"github.com/google/uuid"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/collab"
	"vltstudio/backend/internal/model"
)

type createCloudProjectRequest struct {
	Title             string `json:"title"`
	FormatVersion     int    `json:"format_version"`
	EngineVersion     string `json:"engine_version"`
	MinimumAppVersion string `json:"minimum_app_version"`
}

type updateCloudProjectRequest struct {
	Title             *string `json:"title"`
	EngineVersion     *string `json:"engine_version"`
	MinimumAppVersion *string `json:"minimum_app_version"`
}

type putProjectMemberRequest struct {
	Role       string `json:"role"`
	ColorIndex int16  `json:"color_index"`
}

type transferProjectOwnershipRequest struct {
	TargetUserID uuid.UUID `json:"targetUserId"`
}

type createProjectInviteRequest struct {
	Role             string `json:"role"`
	TargetEmail      string `json:"targetEmail"`
	ExpiresInSeconds int64  `json:"expiresInSeconds"`
}

type acceptProjectInviteRequest struct {
	Token string `json:"token"`
}

type appendProjectOperationRequest struct {
	OpID          string                     `json:"opId"`
	TransactionID string                     `json:"transactionId"`
	Kind          string                     `json:"kind"`
	SchemaVersion int                        `json:"schemaVersion"`
	BaseSeq       int64                      `json:"baseServerSeq"`
	Payload       json.RawMessage            `json:"payload"`
	Preconditions []fieldPreconditionRequest `json:"preconditions"`
	TouchedFields []string                   `json:"touchedFields"`
}

type fieldPreconditionRequest struct {
	Kind        string `json:"kind"`
	FieldKey    string `json:"fieldKey"`
	OperationID string `json:"operationId"`
}

type sessionCompatibilityRequest struct {
	AppVersion           string `json:"appVersion"`
	EngineVersion        string `json:"engineVersion"`
	CommandSchemaVersion int    `json:"commandSchemaVersion"`
	ProjectFormatVersion int    `json:"projectFormatVersion"`
}

func (input sessionCompatibilityRequest) compatibility() collab.ClientCompatibility {
	return collab.ClientCompatibility{
		AppVersion: input.AppVersion, EngineVersion: input.EngineVersion,
		CommandSchemaVersion: input.CommandSchemaVersion,
		ProjectFormatVersion: input.ProjectFormatVersion,
	}
}

type startProjectSessionRequest struct {
	sessionCompatibilityRequest
	Mode string `json:"mode"`
}

type joinProjectSessionRequest struct{ sessionCompatibilityRequest }

type handoffProjectHostRequest struct {
	TargetMemberID uuid.UUID `json:"target_member_id"`
}

type trackLeaseRequest struct {
	TrackID    uuid.UUID `json:"track_id"`
	TTLSeconds int       `json:"ttl_seconds"`
}

type renewTrackLeaseRequest struct {
	TTLSeconds int `json:"ttl_seconds"`
}

func (s *Server) cloudProjects(w http.ResponseWriter, r *http.Request) {
	projects, err := s.Collab.ListProjects(r.Context(), userFrom(r).ID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"projects": projects})
}

func (s *Server) createCloudProject(w http.ResponseWriter, r *http.Request) {
	var input createCloudProjectRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	project, err := s.Collab.CreateProject(r.Context(), collab.CreateProjectInput{
		OwnerUserID: userFrom(r).ID, Title: input.Title, FormatVersion: input.FormatVersion,
		EngineVersion: input.EngineVersion, MinimumAppVersion: input.MinimumAppVersion,
	})
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusCreated, project)
}

func (s *Server) cloudProject(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	project, err := s.Collab.GetProject(r.Context(), projectID, userFrom(r).ID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, project)
}

func (s *Server) updateCloudProject(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	var input updateCloudProjectRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	project, err := s.Collab.UpdateProject(r.Context(), projectID, userFrom(r).ID, collab.UpdateProjectInput{
		Title: input.Title, EngineVersion: input.EngineVersion, MinimumAppVersion: input.MinimumAppVersion,
	})
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, project)
}

func (s *Server) archiveCloudProject(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	if err := s.Collab.ArchiveProject(r.Context(), projectID, userFrom(r).ID); err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if s.Rooms != nil {
		s.Rooms.ShutdownProject(projectID, collab.RoomClose{
			Code: "project_archived", Reason: "cloud project archived",
		})
	}
	if s.Hashes != nil {
		s.Hashes.ClearProject(projectID)
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) publishCloudProject(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	project, err := s.Collab.CompletePublication(r.Context(), projectID, userFrom(r).ID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, project)
}

func (s *Server) bootstrapCloudProject(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	afterSeq, valid := parseNonNegativeQuery(w, r, "after_seq", 0)
	if !valid {
		return
	}
	limit, valid := parseNonNegativeQuery(w, r, "limit", collab.DefaultBootstrapLimit)
	if !valid {
		return
	}
	bootstrap, err := s.Collab.Bootstrap(r.Context(), projectID, userFrom(r).ID, afterSeq, int(limit))
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, bootstrap)
}

func (s *Server) projectMembers(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	members, err := s.Collab.ListMembers(r.Context(), projectID, userFrom(r).ID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"members": members})
}

func (s *Server) putProjectMember(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	memberID, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var input putProjectMemberRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	member, err := s.Collab.PutMember(r.Context(), projectID, userFrom(r).ID, memberID,
		strings.TrimSpace(input.Role), input.ColorIndex)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if s.Rooms != nil {
		s.Rooms.DisconnectProjectUser(projectID, memberID, collab.RoomClose{
			Code: "role_changed", Reason: "project role changed",
		})
	}
	writeJSON(w, http.StatusOK, member)
}

func (s *Server) removeProjectMember(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	memberID, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	if err := s.Collab.RemoveMember(r.Context(), projectID, userFrom(r).ID, memberID); err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if s.Rooms != nil {
		s.Rooms.DisconnectProjectUser(projectID, memberID, collab.RoomClose{
			Code: "member_removed", Reason: "project membership removed",
		})
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) transferProjectOwnership(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	var input transferProjectOwnershipRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	result, err := s.Collab.TransferOwnership(r.Context(), projectID, userFrom(r).ID, input.TargetUserID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if s.Rooms != nil {
		s.Rooms.DisconnectProjectUser(projectID, result.PreviousOwner, collab.RoomClose{
			Code: "role_changed", Reason: "project ownership transferred",
		})
		s.Rooms.DisconnectProjectUser(projectID, result.NewOwner, collab.RoomClose{
			Code: "role_changed", Reason: "project ownership transferred",
		})
	}
	writeJSON(w, http.StatusOK, result)
}

func (s *Server) createProjectInvite(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	var input createProjectInviteRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.ExpiresInSeconds < 0 || input.ExpiresInSeconds > int64(collab.MaxInviteTTL/time.Second) {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Invite expiry is outside the supported range.", nil)
		return
	}
	var ttl time.Duration
	if input.ExpiresInSeconds != 0 {
		ttl = time.Duration(input.ExpiresInSeconds) * time.Second
	}
	invite, err := s.Collab.CreateInvite(r.Context(), collab.CreateInviteInput{
		ProjectID: projectID, ActorUserID: userFrom(r).ID,
		ActorDeviceID: deviceFrom(r).ID, ActorSessionID: collaborationActorSessionID(r),
		Role: strings.TrimSpace(input.Role), TargetEmail: input.TargetEmail, TTL: ttl,
	})
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusCreated, invite)
}

func (s *Server) revokeProjectInvite(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	inviteID, ok := parseUUIDParam(w, r, "inviteID")
	if !ok {
		return
	}
	if err := s.Collab.RevokeInvite(r.Context(), projectID, inviteID, userFrom(r).ID); err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) acceptProjectInvite(w http.ResponseWriter, r *http.Request) {
	var input acceptProjectInviteRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	project, err := s.Collab.AcceptInvite(r.Context(), input.Token, userFrom(r).ID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, project)
}

func (s *Server) appendProjectOperation(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	var input appendProjectOperationRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	opID, err := collab.ParseOperationUUID(input.OpID, false)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	transactionID, err := collab.ParseOperationUUID(input.TransactionID, true)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	preconditions := make([]collab.FieldPrecondition, 0, len(input.Preconditions))
	for _, precondition := range input.Preconditions {
		operationID, err := collab.ParseOperationUUID(precondition.OperationID, false)
		if err != nil {
			s.writeCollaborationError(w, r, err)
			return
		}
		preconditions = append(preconditions, collab.FieldPrecondition{
			Kind: precondition.Kind, FieldKey: precondition.FieldKey, OperationID: *operationID,
		})
	}
	releasePermit, permitErr := s.Hashes.AcquireAppendPermit(projectID)
	if permitErr != nil {
		writeError(w, r, http.StatusConflict, "hash_consensus_pending",
			"Project writes are paused while clients resynchronize.", nil)
		return
	}
	operation, duplicate, err := s.Collab.AppendOperation(r.Context(), collab.AppendOperationInput{
		ProjectID: projectID, ActorUserID: userFrom(r).ID, ActorDeviceID: deviceFrom(r).ID,
		ActorSessionID: collaborationActorSessionID(r),
		OpID:           *opID, TransactionID: transactionID, Kind: input.Kind,
		SchemaVersion: input.SchemaVersion, BaseSeq: input.BaseSeq, Payload: input.Payload,
		Preconditions: preconditions, TouchedFields: input.TouchedFields,
	})
	releasePermit()
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	status := http.StatusCreated
	if duplicate {
		status = http.StatusOK
	} else if s.Rooms != nil {
		s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{
			Data: collaborationCommittedEnvelope(operation),
		})
	}
	writeJSON(w, status, map[string]any{"operation": operation, "duplicate": duplicate})
}

func (s *Server) projectOperationStatus(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	operationID, ok := parseUUIDParam(w, r, "opID")
	if !ok {
		return
	}
	if operationID == uuid.Nil {
		writeError(w, r, http.StatusBadRequest, "invalid_id", "Invalid identifier.", nil)
		return
	}
	status, err := s.Collab.GetOperationStatus(r.Context(), projectID,
		userFrom(r).ID, operationID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func (s *Server) activeProjectSession(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	state, err := s.Collab.GetActiveSession(r.Context(), projectID, userFrom(r).ID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, state)
}

func (s *Server) startProjectSession(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	var input startProjectSessionRequest
	if !decodeSessionCompatibilityJSON(w, r, &input) {
		return
	}
	if strings.TrimSpace(input.Mode) == "" {
		input.Mode = model.SessionModeIndependent
	}
	state, err := s.Collab.StartSessionCompatible(r.Context(), projectID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r),
		input.Mode, input.compatibility())
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusCreated, state)
}

func (s *Server) joinProjectSession(w http.ResponseWriter, r *http.Request) {
	s.sessionMemberAction(w, r, "join")
}

func (s *Server) leaveProjectSession(w http.ResponseWriter, r *http.Request) {
	s.sessionMemberAction(w, r, "leave")
}

func (s *Server) heartbeatProjectSession(w http.ResponseWriter, r *http.Request) {
	s.sessionMemberAction(w, r, "heartbeat")
}

func (s *Server) sessionMemberAction(w http.ResponseWriter, r *http.Request, action string) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	userID, deviceID := userFrom(r).ID, deviceFrom(r).ID
	authSessionID := collaborationActorSessionID(r)
	switch action {
	case "join":
		var input joinProjectSessionRequest
		if !decodeSessionCompatibilityJSON(w, r, &input) {
			return
		}
		state, err := s.Collab.JoinSessionCompatible(r.Context(), projectID,
			sessionID, userID, deviceID, authSessionID, input.compatibility())
		if err != nil {
			s.writeCollaborationError(w, r, err)
			return
		}
		writeJSON(w, http.StatusOK, state)
	case "leave":
		state, err := s.Collab.LeaveSession(r.Context(), projectID, sessionID,
			userID, deviceID, authSessionID)
		if err != nil {
			s.writeCollaborationError(w, r, err)
			return
		}
		writeJSON(w, http.StatusOK, state)
	case "heartbeat":
		member, err := s.Collab.HeartbeatSession(r.Context(), projectID, sessionID,
			userID, deviceID, authSessionID)
		if err != nil {
			s.writeCollaborationError(w, r, err)
			return
		}
		writeJSON(w, http.StatusOK, member)
	}
}

func (s *Server) handoffProjectSessionHost(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	var input handoffProjectHostRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	state, err := s.Collab.HandoffHost(r.Context(), projectID, sessionID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r),
		input.TargetMemberID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if state.Session.HostMemberID != nil && s.Rooms != nil {
		payload, _ := json.Marshal(map[string]any{
			"hostParticipantId": *state.Session.HostMemberID, "reason": "manual",
		})
		s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: collaborationEnvelope("session.host_changed", payload, uuid.Nil, nil, 0)})
	}
	writeJSON(w, http.StatusOK, state)
}

func (s *Server) endProjectSession(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	retryDelay := time.Duration(s.Config.CollabSnapshotRetrySeconds) * time.Second
	result, err := s.Collab.BeginEndSession(r.Context(), projectID, sessionID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), retryDelay)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if result.Finalized {
		s.publishSnapshotFinalization(&collab.SnapshotFinalization{
			ProjectID: result.ProjectID, SessionID: result.SessionID,
			FinalSeq: result.FinalSeq,
		})
	} else {
		s.publishSessionEnding(result.ProjectID, result.SessionID, result.FinalSeq)
		if result.Dispatch != nil {
			s.publishSnapshotDispatch(*result.Dispatch)
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) acquireTrackLease(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	var input trackLeaseRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	ttl, ok := s.collaborationLeaseTTL(w, r, input.TTLSeconds)
	if !ok {
		return
	}
	lease, err := s.Collab.AcquireTrackLease(r.Context(), projectID, sessionID, input.TrackID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), ttl)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusCreated, lease)
}

func (s *Server) renewTrackLease(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	leaseID, ok := parseUUIDParam(w, r, "leaseID")
	if !ok {
		return
	}
	var input renewTrackLeaseRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	ttl, ok := s.collaborationLeaseTTL(w, r, input.TTLSeconds)
	if !ok {
		return
	}
	lease, err := s.Collab.RenewTrackLease(r.Context(), projectID, sessionID, leaseID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), ttl)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, lease)
}

func (s *Server) releaseTrackLease(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	leaseID, ok := parseUUIDParam(w, r, "leaseID")
	if !ok {
		return
	}
	if err := s.Collab.ReleaseTrackLease(r.Context(), projectID, sessionID, leaseID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r)); err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func collaborationActorSessionID(r *http.Request) uuid.UUID {
	claims, ok := r.Context().Value(ctxDesktopClaims).(auth.Claims)
	if !ok {
		return uuid.Nil
	}
	return claims.SessionID
}

func collaborationSessionIDs(w http.ResponseWriter, r *http.Request) (uuid.UUID, uuid.UUID, bool) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return uuid.Nil, uuid.Nil, false
	}
	sessionID, ok := parseUUIDParam(w, r, "sessionID")
	return projectID, sessionID, ok
}

func (s *Server) collaborationLeaseTTL(w http.ResponseWriter, r *http.Request, requested int) (time.Duration, bool) {
	if requested == 0 {
		requested = s.Config.CollabLeaseSeconds
		if requested == 0 {
			requested = int(collab.DefaultLeaseTTL / time.Second)
		}
	}
	if requested < int(collab.MinLeaseTTL/time.Second) || requested > int(collab.MaxLeaseTTL/time.Second) {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Lease expiry is outside the supported range.", nil)
		return 0, false
	}
	return time.Duration(requested) * time.Second, true
}

func parseNonNegativeQuery(w http.ResponseWriter, r *http.Request, name string, fallback int64) (int64, bool) {
	raw := strings.TrimSpace(r.URL.Query().Get(name))
	if raw == "" {
		return fallback, true
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || value < 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", name+" must be a non-negative integer.", nil)
		return 0, false
	}
	return value, true
}

func (s *Server) writeCollaborationError(w http.ResponseWriter, r *http.Request, err error) {
	var precondition *collab.PreconditionError
	if errors.As(err, &precondition) {
		writeJSON(w, http.StatusConflict, map[string]any{
			"code": "operation_precondition_failed", "message": precondition.Error(),
			"request_id": middleware.GetReqID(r.Context()), "conflicts": precondition.Conflicts,
		})
		return
	}
	var held *collab.LeaseHeldError
	if errors.As(err, &held) {
		writeJSON(w, http.StatusConflict, map[string]any{
			"code": "track_lease_held", "message": held.Error(),
			"request_id": middleware.GetReqID(r.Context()), "holder_member_id": held.HolderMemberID,
			"expires_at": held.ExpiresAt,
		})
		return
	}
	switch {
	case errors.Is(err, collab.ErrValidation):
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", err.Error(), nil)
	case errors.Is(err, collab.ErrVersionMismatch):
		writeError(w, r, http.StatusUnprocessableEntity, "version_mismatch",
			"This app or engine version is incompatible with the cloud project.", nil)
	case errors.Is(err, collab.ErrNotFound):
		writeError(w, r, http.StatusNotFound, "collaboration_not_found", "Collaboration resource was not found.", nil)
	case errors.Is(err, collab.ErrForbidden):
		writeError(w, r, http.StatusForbidden, "collaboration_forbidden", "You do not have permission for this action.", nil)
	case errors.Is(err, collab.ErrProjectInactive):
		writeError(w, r, http.StatusConflict, "project_not_active", "The cloud project is not currently editable.", nil)
	case errors.Is(err, collab.ErrOperationIDReuse):
		writeError(w, r, http.StatusConflict, "operation_id_reused", "Operation identifier was reused with different content.", nil)
	case errors.Is(err, collab.ErrBaseSeqAhead):
		writeError(w, r, http.StatusConflict, "base_sequence_ahead", "Client sequence is ahead of the project.", nil)
	case errors.Is(err, collab.ErrBaseSeqMismatch):
		writeError(w, r, http.StatusConflict, "base_sequence_stale", "Recording commit must be rebuilt from the current project sequence.", nil)
	case errors.Is(err, collab.ErrLiveSessionRequired):
		writeError(w, r, http.StatusConflict, "live_session_required", "Join the active collaboration session from this device before editing.", nil)
	case errors.Is(err, collab.ErrEntityDeleted):
		writeError(w, r, http.StatusConflict, "entity_deleted", "The command targets an entity that is currently deleted.", nil)
	case errors.Is(err, collab.ErrSessionEnded):
		writeError(w, r, http.StatusConflict, "session_ended", "The collaboration session has ended.", nil)
	case errors.Is(err, collab.ErrSessionFull):
		writeError(w, r, http.StatusConflict, "session_full", "The collaboration session has reached its participant limit.", nil)
	case errors.Is(err, collab.ErrLeaseExpired):
		writeError(w, r, http.StatusConflict, "track_lease_expired", "The track lease expired and must be acquired again.", nil)
	case errors.Is(err, collab.ErrLeaseRequired):
		writeError(w, r, http.StatusConflict, "track_lease_required", "Acquire an active recording lease for every target track before committing the recording.", nil)
	case errors.Is(err, collab.ErrAssetUnavailable):
		writeError(w, r, http.StatusConflict, "asset_incomplete", "Complete and verify every referenced asset before committing the operation.", nil)
	case errors.Is(err, collab.ErrInviteExpired):
		writeError(w, r, http.StatusGone, "invite_expired", "The project invitation has expired.", nil)
	case errors.Is(err, collab.ErrInviteUsed):
		writeError(w, r, http.StatusConflict, "invite_unavailable", "The project invitation is no longer available.", nil)
	case errors.Is(err, collab.ErrConflict):
		writeError(w, r, http.StatusConflict, "collaboration_conflict", "The collaboration state changed. Refresh and try again.", nil)
	default:
		// Collaboration errors may originate from project payload validation,
		// object-storage URLs, database row details, or local cache paths. Keep
		// diagnostics correlation-safe without ever serializing that content.
		log.Printf("collaboration request failed: request_id=%s error_type=%T",
			middleware.GetReqID(r.Context()), err)
		writeError(w, r, http.StatusInternalServerError, "collaboration_failed", "Collaboration request could not be completed.", nil)
	}
}

func decodeSessionCompatibilityJSON(w http.ResponseWriter, r *http.Request,
	out any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, maxJSONBody)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(out); err != nil {
		if errors.Is(err, io.EOF) {
			writeError(w, r, http.StatusUnprocessableEntity, "version_mismatch",
				"Session compatibility information is required.", nil)
			return false
		}
		writeError(w, r, http.StatusBadRequest, "invalid_request",
			"Invalid request body.", nil)
		return false
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, r, http.StatusBadRequest, "invalid_request",
			"Only one JSON value is allowed.", nil)
		return false
	}
	return true
}
