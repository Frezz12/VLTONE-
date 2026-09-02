package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/coder/websocket"
	"github.com/google/uuid"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/collab"
	"vltstudio/backend/internal/model"
)

const (
	collaborationMaximumMessageBytes = 1 << 20
	collaborationHelloTimeout        = 10 * time.Second
	collaborationWriteTimeout        = 10 * time.Second
	collaborationPingInterval        = 25 * time.Second
	collaborationHeartbeatInterval   = 15 * time.Second
)

var collaborationSemanticID = regexp.MustCompile(`^[A-Za-z0-9_.:-]+$`)

type collaborationClientEnvelope struct {
	Protocol      string          `json:"protocol"`
	Type          string          `json:"type"`
	MessageID     string          `json:"messageId"`
	ParticipantID string          `json:"participantId,omitempty"`
	EphemeralSeq  *uint64         `json:"ephemeralSeq,omitempty"`
	SentAtMS      int64           `json:"sentAtMs"`
	Payload       json.RawMessage `json:"payload"`
}

type collaborationServerEnvelope struct {
	Protocol      string          `json:"protocol"`
	Type          string          `json:"type"`
	MessageID     string          `json:"messageId"`
	ParticipantID string          `json:"participantId,omitempty"`
	EphemeralSeq  *uint64         `json:"ephemeralSeq,omitempty"`
	SentAtMS      int64           `json:"sentAtMs,omitempty"`
	ServerTimeMS  int64           `json:"serverTimeMs"`
	Payload       json.RawMessage `json:"payload"`
}

type collaborationHello struct {
	AppVersion           string  `json:"appVersion"`
	EngineVersion        string  `json:"engineVersion"`
	CommandSchemaVersion int     `json:"commandSchemaVersion"`
	ProjectFormatVersion int     `json:"projectFormatVersion"`
	AfterServerSeq       int64   `json:"afterServerSeq"`
	StateHash            *string `json:"stateHash"`
}

type collaborationParticipant struct {
	ParticipantID string `json:"participantId"`
	UserID        string `json:"userId"`
	Nickname      string `json:"nickname"`
	Role          string `json:"role"`
	Color         string `json:"color"`
	Host          bool   `json:"host"`
	JoinedAtMS    int64  `json:"joinedAtMs"`
}

type collaborationWirePrecondition struct {
	Kind        string `json:"kind"`
	FieldKey    string `json:"fieldKey"`
	OperationID string `json:"operationId"`
}

type collaborationWireCommand struct {
	SchemaVersion int                             `json:"schemaVersion"`
	OpID          string                          `json:"opId"`
	TransactionID json.RawMessage                 `json:"transactionId"`
	BaseServerSeq int64                           `json:"baseServerSeq"`
	Kind          string                          `json:"kind"`
	Payload       json.RawMessage                 `json:"payload"`
	Preconditions []collaborationWirePrecondition `json:"preconditions"`
	TouchedFields []string                        `json:"touchedFields"`
}

type collaborationOpSubmit struct {
	Command json.RawMessage `json:"command"`
}

type collaborationSafePresence struct {
	Surface      string   `json:"surface"`
	Precision    string   `json:"precision"`
	TargetID     string   `json:"targetId,omitempty"`
	TrackID      string   `json:"trackId,omitempty"`
	ClipID       string   `json:"clipId,omitempty"`
	Lane         string   `json:"lane,omitempty"`
	ControlID    string   `json:"controlId,omitempty"`
	TimeSeconds  *float64 `json:"timeSeconds,omitempty"`
	Beat         *float64 `json:"beat,omitempty"`
	Pitch        *int     `json:"pitch,omitempty"`
	LaneFraction *float64 `json:"laneFraction,omitempty"`
	U            *float64 `json:"u,omitempty"`
	V            *float64 `json:"v,omitempty"`
	Phase        string   `json:"phase,omitempty"`
	Button       string   `json:"button,omitempty"`
	Gesture      string   `json:"gesture,omitempty"`
	EntityIDs    []string `json:"entityIds,omitempty"`
}

type collaborationTransportState struct {
	Playing           bool    `json:"playing"`
	PositionSeconds   float64 `json:"positionSeconds"`
	MonotonicAnchorMS int64   `json:"monotonicAnchorMs"`
	Rate              float64 `json:"rate"`
}

type collaborationTransportFollow struct {
	Enabled             bool            `json:"enabled"`
	TargetParticipantID json.RawMessage `json:"targetParticipantId"`
}

type collaborationLeaseRequest struct {
	TrackID string `json:"trackId"`
	LeaseID string `json:"leaseId,omitempty"`
}

type collaborationHandoffRequest struct {
	ParticipantID string `json:"participantId"`
}

type collaborationSnapshotHash struct {
	RoundID   string `json:"roundId"`
	ServerSeq int64  `json:"serverSeq"`
	SHA256    string `json:"sha256"`
}

type collaborationRoomConnection struct {
	server        *Server
	connection    *websocket.Conn
	subscription  *collab.RoomSubscription
	projectID     uuid.UUID
	sessionID     uuid.UUID
	participantID uuid.UUID
	userID        uuid.UUID
	deviceID      uuid.UUID
	authSessionID uuid.UUID

	rateMu        sync.Mutex
	lastEphemeral map[string]time.Time
}

type collaborationLiveActorState struct {
	globalEnabled        bool
	userActive           bool
	entitled             bool
	deviceActive         bool
	desktopSessionActive bool
	projectRole          string
	participantActive    bool
}

func collaborationLiveActorDecision(state collaborationLiveActorState,
	stateErr error) (collab.RoomClose, bool) {
	if stateErr != nil {
		return collab.RoomClose{
			Code:   "collaboration_access_unavailable",
			Reason: "collaboration actor state unavailable",
		}, false
	}
	if !state.globalEnabled {
		return collab.RoomClose{
			Code:   "collaboration_access_disabled",
			Reason: "account collaboration access revoked",
		}, false
	}
	if !state.userActive {
		return collab.RoomClose{
			Code: "account_suspended", Reason: "account is unavailable",
		}, false
	}
	if !state.entitled {
		return collab.RoomClose{
			Code:   "collaboration_access_disabled",
			Reason: "account collaboration access revoked",
		}, false
	}
	if !state.deviceActive {
		return collab.RoomClose{
			Code: "device_revoked", Reason: "device has been revoked",
		}, false
	}
	if !state.desktopSessionActive {
		return collab.RoomClose{
			Code:   "desktop_session_revoked",
			Reason: "desktop session has been revoked",
		}, false
	}
	if !state.participantActive ||
		!collab.RoleAllows(state.projectRole, collab.PermissionView) {
		return collab.RoomClose{
			Code: "member_removed", Reason: "project membership has been revoked",
		}, false
	}
	return collab.RoomClose{}, true
}

func enforceCollaborationSubscriptionAccess(
	subscription *collab.RoomSubscription, state collaborationLiveActorState,
	expectedRole string, stateErr error) (collab.RoomClose, bool) {
	closeInfo, allowed := collaborationLiveActorDecision(state, stateErr)
	if allowed && state.projectRole != expectedRole {
		closeInfo = collab.RoomClose{
			Code: "role_changed", Reason: "project role changed during connection",
		}
		allowed = false
	}
	if !allowed {
		subscription.Close(closeInfo)
	}
	return closeInfo, allowed
}

func (s *Server) collaborationLiveActorState(ctx context.Context, projectID,
	sessionID, participantID, userID, deviceID, desktopSessionID uuid.UUID,
	now time.Time) (collaborationLiveActorState, error) {
	state := collaborationLiveActorState{globalEnabled: s.Config.CollaborationEnabled}
	var user struct {
		Status               string
		CollaborationEnabled bool
	}
	err := s.DB.WithContext(ctx).Model(&model.User{}).
		Select("status", "collaboration_enabled").Where("id = ?", userID).
		Take(&user).Error
	if recordNotFound(err) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	state.userActive = user.Status == model.UserActive
	state.entitled = user.CollaborationEnabled || s.collabAllowedUsers[userID]
	if !state.userActive || !state.entitled || !state.globalEnabled {
		return state, nil
	}

	var device model.Device
	err = s.DB.WithContext(ctx).
		Select("id").Where("id = ? AND user_id = ? AND revoked_at IS NULL",
		deviceID, userID).Take(&device).Error
	if recordNotFound(err) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	state.deviceActive = true

	var desktopSession model.DesktopSession
	err = s.DB.WithContext(ctx).Select("id").Where(
		"id = ? AND user_id = ? AND device_id = ? AND revoked_at IS NULL AND expires_at > ?",
		desktopSessionID, userID, deviceID, now.UTC()).Take(&desktopSession).Error
	if recordNotFound(err) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	state.desktopSessionActive = true

	project, err := s.Collab.GetProject(ctx, projectID, userID)
	if errors.Is(err, collab.ErrForbidden) || errors.Is(err, collab.ErrNotFound) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	state.projectRole = project.Role
	if !collab.RoleAllows(state.projectRole, collab.PermissionView) {
		return state, nil
	}

	var participant struct{ ID uuid.UUID }
	err = s.DB.WithContext(ctx).Table("project_session_members AS members").
		Select("members.id").
		Joins("JOIN project_live_sessions AS sessions ON sessions.id = members.session_id").
		Where(`members.id = ? AND members.session_id = ? AND members.user_id = ?
			AND members.device_id = ? AND members.desktop_session_id = ?
			AND members.left_at IS NULL AND sessions.project_id = ?
			AND sessions.status IN ?`, participantID, sessionID, userID, deviceID,
			desktopSessionID, projectID,
			[]string{model.ProjectSessionActive, model.ProjectSessionEnding}).
		Take(&participant).Error
	if recordNotFound(err) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	state.participantActive = true
	return state, nil
}

func (s *Server) collaborationLive(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	user, device := userFrom(r), deviceFrom(r)
	claims, ok := r.Context().Value(ctxDesktopClaims).(auth.Claims)
	if !ok || claims.SessionID == uuid.Nil {
		writeError(w, r, http.StatusUnauthorized, "desktop_session_revoked",
			"Desktop session has been revoked.", nil)
		return
	}
	active, err := s.Collab.GetActiveSession(r.Context(), projectID, user.ID)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}

	connection, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		Subprotocols:    []string{collab.CollaborationProtocol},
		CompressionMode: websocket.CompressionDisabled,
	})
	if err != nil {
		return
	}
	connection.SetReadLimit(collaborationMaximumMessageBytes)
	if connection.Subprotocol() != collab.CollaborationProtocol {
		_ = connection.Close(websocket.StatusProtocolError,
			"vlt-collab-v2 subprotocol required")
		return
	}

	helloContext, cancelHello := context.WithTimeout(context.Background(),
		collaborationHelloTimeout)
	messageType, rawHello, err := connection.Read(helloContext)
	cancelHello()
	if err != nil || messageType != websocket.MessageText {
		_ = connection.Close(websocket.StatusProtocolError, "hello required")
		return
	}
	var helloEnvelope collaborationClientEnvelope
	if err := decodeCollaborationJSON(rawHello, &helloEnvelope); err != nil ||
		validateCollaborationEnvelope(helloEnvelope, "hello", false) != nil {
		_ = connection.Close(websocket.StatusProtocolError, "invalid hello")
		return
	}
	var hello collaborationHello
	if err := decodeCollaborationJSON(helloEnvelope.Payload, &hello); err != nil ||
		validateCollaborationHello(hello) != nil {
		_ = connection.Close(websocket.StatusPolicyViolation,
			"incompatible collaboration client")
		return
	}

	joinContext, cancelJoin := context.WithTimeout(context.Background(),
		collaborationWriteTimeout)
	joined, err := s.Collab.JoinSessionCompatible(joinContext, projectID,
		active.Session.ID, user.ID, device.ID, claims.SessionID,
		collab.ClientCompatibility{
			AppVersion: hello.AppVersion, EngineVersion: hello.EngineVersion,
			CommandSchemaVersion: hello.CommandSchemaVersion,
			ProjectFormatVersion: hello.ProjectFormatVersion,
		})
	cancelJoin()
	if err != nil {
		_ = connection.Close(websocket.StatusPolicyViolation,
			"session membership unavailable")
		return
	}
	joinedTransportOwned := false
	defer func() {
		if joinedTransportOwned {
			return
		}
		cleanupContext, cancelCleanup := context.WithTimeout(context.Background(),
			collaborationWriteTimeout)
		_, _ = s.Collab.LeaveSession(cleanupContext, projectID, joined.Session.ID,
			user.ID, device.ID, claims.SessionID)
		cancelCleanup()
	}()
	stateContext, cancelState := context.WithTimeout(context.Background(),
		collaborationWSDBTimeout(s.Config.CollabWSDBTimeoutSeconds))
	participants, participant, role, err := s.collaborationParticipantViews(
		stateContext, projectID, joined, user.ID, device.ID)
	if err != nil {
		cancelState()
		_ = connection.Close(websocket.StatusInternalError,
			"participant state unavailable")
		return
	}
	project, err := s.Collab.GetProject(stateContext, projectID, user.ID)
	cancelState()
	if err != nil {
		_ = connection.Close(websocket.StatusPolicyViolation,
			"project access unavailable")
		return
	}
	if s.Rooms == nil {
		_ = connection.Close(websocket.StatusTryAgainLater,
			"collaboration room bus unavailable")
		return
	}
	participantID, _ := uuid.Parse(participant.ParticipantID)
	subscription, err := s.Rooms.Subscribe(projectID, user.ID, device.ID,
		claims.SessionID, participantID, collab.DefaultRoomDurableQueue,
		normalizedRoomQueueBytes(s.Config.CollabRoomQueueBytes))
	if err != nil {
		_ = connection.Close(websocket.StatusTryAgainLater,
			"collaboration room unavailable")
		return
	}
	actorContext, cancelActor := context.WithTimeout(
		context.Background(), collaborationWSDBTimeout(
			s.Config.CollabWSDBTimeoutSeconds))
	actorState, actorErr := s.collaborationLiveActorState(actorContext, projectID,
		joined.Session.ID, participantID, user.ID, device.ID, claims.SessionID,
		time.Now().UTC())
	cancelActor()
	if closeInfo, allowed := enforceCollaborationSubscriptionAccess(subscription,
		actorState, role, actorErr); !allowed {
		status := websocket.StatusPolicyViolation
		if actorErr != nil {
			status = websocket.StatusTryAgainLater
		}
		_ = connection.Close(status, safeWebSocketReason(closeInfo.Reason))
		return
	}
	roundContext, cancelRound := context.WithTimeout(context.Background(),
		collaborationWriteTimeout)
	hashRound, err := s.prepareHashRound(roundContext, projectID, joined)
	cancelRound()
	if err != nil {
		subscription.Close(collab.RoomClose{Code: "hash_unavailable",
			Reason: "state hash round unavailable"})
		_ = connection.Close(websocket.StatusInternalError,
			"state hash round unavailable")
		return
	}
	readOnly := !collab.RoleAllows(role, collab.PermissionEdit)
	blockedReason := "hash_consensus_required"
	if readOnly {
		blockedReason = "role_read_only"
	}
	var hashRoundPayload any
	if hashRound.view.RoundID != uuid.Nil {
		hashRoundPayload = map[string]any{
			"roundId": hashRound.view.RoundID, "sessionId": hashRound.view.SessionID,
			"serverSeq":  hashRound.view.ServerSeq,
			"deadlineMs": hashRound.view.Deadline.UnixMilli(),
		}
	}
	welcomePayload, _ := json.Marshal(map[string]any{
		"projectId": projectID, "sessionId": joined.Session.ID,
		"participant": participant, "hostParticipantId": joined.Session.HostMemberID,
		"headSeq": project.Project.HeadSeq, "readOnly": readOnly,
		"writeBlockedReason": blockedReason, "hashRound": hashRoundPayload,
		"participants": participants,
		"limits": map[string]any{
			"maxParticipants": s.Config.CollabMaxParticipants,
			"cursorHz":        20, "transportHz": 10,
			"maxMessageBytes": collaborationMaximumMessageBytes,
			"roomQueueBytes":  normalizedRoomQueueBytes(s.Config.CollabRoomQueueBytes),
		},
	})
	if err := writeCollaborationSocket(connection, collaborationEnvelope(
		"welcome", welcomePayload, uuid.Nil, nil, 0)); err != nil {
		subscription.Close(collab.RoomClose{Code: "disconnected",
			Reason: "welcome delivery failed"})
		return
	}
	s.publishHashRound(projectID, hashRound)
	roomConnection := &collaborationRoomConnection{
		server: s, connection: connection, subscription: subscription,
		projectID: projectID, sessionID: joined.Session.ID,
		participantID: participantID, userID: user.ID, deviceID: device.ID,
		authSessionID: claims.SessionID, lastEphemeral: make(map[string]time.Time),
	}
	joinedPayload, _ := json.Marshal(participant)
	s.Rooms.Publish(projectID, participantID, collab.RoomMessage{
		Data: collaborationEnvelope("presence.join", joinedPayload, uuid.Nil, nil, 0),
	})
	joinedTransportOwned = true
	roomConnection.run(joined.Session.HostMemberID)
}

func (connection *collaborationRoomConnection) run(initialHost *uuid.UUID) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	errorsChannel := make(chan error, 3)
	go func() { errorsChannel <- connection.writeLoop(ctx) }()
	go func() { errorsChannel <- connection.readLoop(ctx) }()
	go func() { errorsChannel <- connection.heartbeatLoop(ctx) }()
	err := <-errorsChannel
	cancel()

	closeInfo := connection.subscription.CloseInfo()
	if closeInfo.Code == "" {
		closeInfo = collab.RoomClose{Code: "disconnected", Reason: "connection closed"}
	}
	connection.subscription.Close(closeInfo)
	status := collaborationWebSocketCloseStatus(closeInfo, err)
	_ = connection.connection.Close(status, safeWebSocketReason(closeInfo.Reason))
	if closeInfo.Code == "connection_replaced" {
		// The new socket owns the same user/device membership. Leaving here would
		// invalidate the replacement immediately and recreate the ghost/rejoin race.
		return
	}
	if closeInfo.Code == "heartbeat_timeout" {
		// Heartbeat timeout is already committed and broadcast by the maintenance
		// reaper before it closes the transport, so repeating LeaveSession here would
		// emit duplicate presence and host events.
		return
	}

	leaveContext, cancelLeave := context.WithTimeout(context.Background(),
		collaborationWriteTimeout)
	state, leaveErr := connection.server.Collab.LeaveSession(leaveContext,
		connection.projectID, connection.sessionID, connection.userID,
		connection.deviceID, connection.authSessionID)
	cancelLeave()
	leftPayload, _ := json.Marshal(map[string]any{
		"participantId": connection.participantID, "reason": closeInfo.Code,
	})
	connection.server.Rooms.Publish(connection.projectID,
		connection.participantID, collab.RoomMessage{
			Data: collaborationEnvelope("presence.leave", leftPayload,
				uuid.Nil, nil, 0),
		})
	currentHost := state.Session.HostMemberID
	hostKnown := leaveErr == nil
	if !hostKnown {
		// Forced eviction commits before RoomBus disconnect and makes the revoked
		// credential unable to call LeaveSession. Read only the exact room row so
		// remaining participants still learn the host elected by Evict*Tx.
		hostContext, cancelHost := context.WithTimeout(context.Background(),
			collaborationWriteTimeout)
		var current model.ProjectSession
		hostErr := connection.server.DB.WithContext(hostContext).
			Select("id", "host_member_id").
			Where("id = ? AND project_id = ? AND status = ?", connection.sessionID,
				connection.projectID, model.ProjectSessionActive).First(&current).Error
		cancelHost()
		if hostErr == nil {
			currentHost = current.HostMemberID
			hostKnown = true
		}
	}
	if hostKnown && !sameOptionalUUID(initialHost, currentHost) {
		hostPayload, _ := json.Marshal(map[string]any{
			"hostParticipantId": currentHost,
			"reason":            "disconnected",
		})
		connection.server.Rooms.Publish(connection.projectID, uuid.Nil,
			collab.RoomMessage{Data: collaborationEnvelope("session.host_changed",
				hostPayload, uuid.Nil, nil, 0)})
	}
	if leaveErr == nil {
		hashContext, cancelHash := context.WithTimeout(context.Background(),
			collaborationWriteTimeout)
		if round, hashErr := connection.server.prepareHashRound(hashContext,
			connection.projectID, state); hashErr == nil {
			connection.server.publishHashRound(connection.projectID, round)
		}
		cancelHash()
	}
}

func collaborationWebSocketCloseStatus(closeInfo collab.RoomClose,
	err error) websocket.StatusCode {
	if closeInfo.Code == "slow_consumer" {
		return websocket.StatusTryAgainLater
	}
	if closeInfo.Code == "device_revoked" ||
		closeInfo.Code == "member_removed" ||
		closeInfo.Code == "account_suspended" ||
		closeInfo.Code == "refresh_token_reused" ||
		closeInfo.Code == "desktop_session_revoked" ||
		closeInfo.Code == "role_changed" ||
		closeInfo.Code == "collaboration_access_disabled" {
		return websocket.StatusPolicyViolation
	}
	if err != nil && websocket.CloseStatus(err) == -1 &&
		!errors.Is(err, context.Canceled) {
		return websocket.StatusGoingAway
	}
	return websocket.StatusNormalClosure
}

func (connection *collaborationRoomConnection) writeLoop(ctx context.Context) error {
	for {
		waitContext, cancel := context.WithTimeout(ctx, collaborationPingInterval)
		message, err := connection.subscription.Next(waitContext)
		cancel()
		if err == nil {
			if err := writeCollaborationSocket(connection.connection, message.Data); err != nil {
				return err
			}
			continue
		}
		if errors.Is(err, context.DeadlineExceeded) && ctx.Err() == nil {
			pingContext, cancelPing := context.WithTimeout(ctx,
				collaborationWriteTimeout)
			err = connection.connection.Ping(pingContext)
			cancelPing()
			if err == nil {
				continue
			}
		}
		return err
	}
}

func (connection *collaborationRoomConnection) heartbeatLoop(ctx context.Context) error {
	ticker := time.NewTicker(collaborationHeartbeatInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			heartbeatContext, cancel := context.WithTimeout(ctx,
				collaborationWriteTimeout)
			_, err := connection.server.Collab.HeartbeatSession(heartbeatContext,
				connection.projectID, connection.sessionID, connection.userID,
				connection.deviceID, connection.authSessionID)
			cancel()
			if err != nil {
				return err
			}
		}
	}
}

func (connection *collaborationRoomConnection) readLoop(ctx context.Context) error {
	violations := 0
	for {
		messageType, data, err := connection.connection.Read(ctx)
		if err != nil {
			return err
		}
		if messageType != websocket.MessageText {
			violations++
			if violations >= 3 {
				return fmt.Errorf("too many non-text collaboration messages")
			}
			continue
		}
		var envelope collaborationClientEnvelope
		messageContext, cancelMessage := context.WithTimeout(ctx,
			collaborationWSDBTimeout(connection.server.Config.CollabWSDBTimeoutSeconds))
		if err := decodeCollaborationJSON(data, &envelope); err != nil {
			violations++
			connection.reject(messageContext, "", "invalid_message",
				"Invalid collaboration message.", false)
		} else if err := connection.handleEnvelope(messageContext, envelope); err != nil {
			violations++
			connection.reject(messageContext, envelope.MessageID, "invalid_message",
				"Invalid collaboration message.", false)
		} else {
			violations = 0
		}
		cancelMessage()
		if violations >= 3 {
			return fmt.Errorf("too many invalid collaboration messages")
		}
	}
}

func (connection *collaborationRoomConnection) handleEnvelope(ctx context.Context,
	envelope collaborationClientEnvelope) error {
	ephemeral := isCollaborationEphemeral(envelope.Type)
	if err := validateCollaborationEnvelope(envelope, "", ephemeral); err != nil {
		return err
	}
	switch envelope.Type {
	case "op.submit":
		return connection.submitOperation(ctx, envelope)
	case "presence.cursor", "presence.click", "presence.selection", "presence.drag":
		if err := validateCollaborationPresence(envelope.Type, envelope.Payload); err != nil {
			return err
		}
		return connection.relayEphemeral(envelope, ephemeralInterval(envelope.Type))
	case "transport.state":
		var payload collaborationTransportState
		if err := decodeCollaborationJSON(envelope.Payload, &payload); err != nil ||
			payload.PositionSeconds < 0 || payload.MonotonicAnchorMS < 0 ||
			payload.Rate < 0.25 || payload.Rate > 4 {
			return errors.New("invalid transport state")
		}
		return connection.relayEphemeral(envelope, 100*time.Millisecond)
	case "transport.follow":
		var payload collaborationTransportFollow
		if err := decodeCollaborationJSON(envelope.Payload, &payload); err != nil ||
			validateOptionalUUIDJSON(payload.TargetParticipantID,
				payload.Enabled) != nil {
			return errors.New("invalid transport follow")
		}
		return connection.relayEphemeral(envelope, 100*time.Millisecond)
	case "lease.acquire", "lease.renew", "lease.release":
		return connection.handleLease(ctx, envelope)
	case "session.handoff":
		return connection.handleHandoff(ctx, envelope)
	case "snapshot.hash":
		var payload collaborationSnapshotHash
		if err := decodeCollaborationJSON(envelope.Payload, &payload); err != nil ||
			uuid.Validate(payload.RoundID) != nil || payload.ServerSeq < 0 ||
			!validSHA256(payload.SHA256) {
			return errors.New("invalid snapshot hash")
		}
		return connection.handleSnapshotHash(ctx, envelope, payload)
	default:
		return errors.New("unsupported collaboration message")
	}
}

func (connection *collaborationRoomConnection) submitOperation(ctx context.Context,
	envelope collaborationClientEnvelope) error {
	var request collaborationOpSubmit
	if err := decodeCollaborationJSON(envelope.Payload, &request); err != nil {
		return err
	}
	command, transactionID, err := parseCollaborationWireCommand(request.Command)
	if err != nil {
		return err
	}
	opID, err := uuid.Parse(command.OpID)
	if err != nil || opID == uuid.Nil {
		return errors.New("invalid operation id")
	}
	preconditions := make([]collab.FieldPrecondition, 0, len(command.Preconditions))
	for _, precondition := range command.Preconditions {
		operationID, err := uuid.Parse(precondition.OperationID)
		if err != nil || operationID == uuid.Nil {
			return errors.New("invalid precondition operation id")
		}
		preconditions = append(preconditions, collab.FieldPrecondition{
			Kind: precondition.Kind, FieldKey: precondition.FieldKey,
			OperationID: operationID,
		})
	}
	operation, duplicate, appendErr := connection.server.appendCollaborationOperation(
		ctx, collab.AppendOperationInput{
			ProjectID: connection.projectID, ActorUserID: connection.userID,
			ActorDeviceID:  connection.deviceID,
			ActorSessionID: connection.authSessionID, OpID: opID,
			TransactionID: transactionID, Kind: command.Kind,
			SchemaVersion: command.SchemaVersion, BaseSeq: command.BaseServerSeq,
			Payload: command.Payload, Preconditions: preconditions,
			TouchedFields: command.TouchedFields,
		})
	if appendErr != nil {
		code, message, retryable := collaborationRejection(appendErr)
		connection.rejectWithOperation(ctx, envelope.MessageID, command.OpID,
			code, message, retryable)
		return nil
	}
	committed := collaborationCommittedEnvelope(operation)
	if duplicate {
		connection.subscription.Deliver(collab.RoomMessage{Data: committed})
	}
	return nil
}

func (connection *collaborationRoomConnection) handleSnapshotHash(ctx context.Context,
	envelope collaborationClientEnvelope, report collaborationSnapshotHash) error {
	state, err := connection.server.Collab.GetActiveSession(ctx,
		connection.projectID, connection.userID)
	if err != nil || state.Session.ID != connection.sessionID ||
		state.Session.HostMemberID == nil {
		return errors.New("snapshot hash has no active host")
	}
	project, err := connection.server.Collab.GetProject(ctx,
		connection.projectID, connection.userID)
	if err != nil {
		return err
	}
	if report.ServerSeq != project.Project.HeadSeq {
		payload, _ := json.Marshal(map[string]any{
			"reason": "sequence_gap", "snapshotSeq": project.Project.SnapshotSeq,
			"headSeq": project.Project.HeadSeq, "readOnly": false,
		})
		connection.subscription.Deliver(collab.RoomMessage{Data: collaborationEnvelope("resync.required", payload, uuid.Nil, nil, 0)})
		if connection.server.metrics != nil {
			connection.server.metrics.resyncs.Add(1)
		}
		return nil
	}
	roundID, _ := uuid.Parse(report.RoundID)
	result := connection.server.Hashes.Report(connection.projectID, roundID,
		connection.sessionID, connection.participantID,
		report.ServerSeq, report.SHA256)
	if result.Decision == collab.HashVerified {
		payload, _ := json.Marshal(map[string]any{
			"roundId": result.Round.RoundID, "serverSeq": result.Round.ServerSeq,
		})
		connection.server.Rooms.Publish(connection.projectID, uuid.Nil,
			collab.RoomMessage{Data: collaborationEnvelope("hash.verified", payload,
				uuid.Nil, nil, 0)})
		return nil
	}
	if result.Decision != collab.HashResync && result.Decision != collab.HashConflict {
		return nil
	}
	readOnly := result.Decision == collab.HashConflict
	if readOnly {
		if err := connection.server.DB.WithContext(ctx).
			Model(&model.CloudProject{}).
			Where("id = ? AND status = ?", connection.projectID,
				model.ProjectActive).
			Updates(map[string]any{"status": model.ProjectConflict,
				"updated_at": time.Now().UTC()}).Error; err != nil {
			return err
		}
	}
	reason := "hash_mismatch"
	if readOnly {
		reason = "conflict"
	}
	payload, _ := json.Marshal(map[string]any{
		"reason": reason, "snapshotSeq": project.Project.SnapshotSeq,
		"headSeq": project.Project.HeadSeq, "readOnly": readOnly,
	})
	connection.server.Rooms.Publish(connection.projectID, uuid.Nil,
		collab.RoomMessage{Data: collaborationEnvelope("resync.required", payload,
			uuid.Nil, nil, 0)})
	if connection.server.metrics != nil {
		connection.server.metrics.resyncs.Add(1)
	}
	if result.Decision == collab.HashResync {
		expected, err := connection.server.connectedEditorParticipants(ctx,
			connection.projectID, state)
		if err != nil {
			return err
		}
		connection.server.publishHashRound(connection.projectID, openedHashRound{
			view: result.Round, expected: expected,
		})
	}
	return nil
}

func (connection *collaborationRoomConnection) handleLease(ctx context.Context,
	envelope collaborationClientEnvelope) error {
	if !cloudRecordingEnabledV1 {
		connection.reject(ctx, envelope.MessageID, "cloud_recording_disabled",
			"Recording is not available in cloud projects.", false)
		return nil
	}
	var request collaborationLeaseRequest
	if err := decodeCollaborationJSON(envelope.Payload, &request); err != nil {
		return err
	}
	trackID, err := uuid.Parse(request.TrackID)
	if err != nil || trackID == uuid.Nil {
		return errors.New("invalid lease track")
	}
	leaseTTL := time.Duration(connection.server.Config.CollabLeaseSeconds) * time.Second
	if leaseTTL == 0 {
		leaseTTL = collab.DefaultLeaseTTL
	}
	var lease model.ProjectTrackLease
	switch envelope.Type {
	case "lease.acquire":
		if request.LeaseID != "" {
			return errors.New("lease id is not accepted for acquire")
		}
		lease, err = connection.server.Collab.AcquireTrackLease(ctx,
			connection.projectID, connection.sessionID, trackID,
			connection.userID, connection.deviceID, connection.authSessionID,
			leaseTTL)
	case "lease.renew":
		leaseID, parseErr := uuid.Parse(request.LeaseID)
		if parseErr != nil || leaseID == uuid.Nil {
			return errors.New("invalid lease id")
		}
		lease, err = connection.server.Collab.RenewTrackLeaseForTrack(ctx,
			connection.projectID, connection.sessionID, leaseID, trackID,
			connection.userID, connection.deviceID, connection.authSessionID,
			leaseTTL)
	case "lease.release":
		leaseID, parseErr := uuid.Parse(request.LeaseID)
		if parseErr != nil || leaseID == uuid.Nil {
			return errors.New("invalid lease id")
		}
		err = connection.server.Collab.ReleaseTrackLeaseForTrack(ctx,
			connection.projectID, connection.sessionID, leaseID, trackID,
			connection.userID, connection.deviceID, connection.authSessionID)
		if err == nil {
			return connection.relayDurable(envelope)
		}
	}
	if err != nil {
		code, message, retryable := collaborationRejection(err)
		payload, _ := json.Marshal(map[string]any{
			"requestMessageId": envelope.MessageID, "code": code,
			"message": message, "retryable": retryable,
		})
		connection.subscription.Deliver(collab.RoomMessage{Data: collaborationEnvelope("lease.denied", payload, uuid.Nil, nil, 0)})
		return nil
	}
	payload, _ := json.Marshal(map[string]any{
		"leaseId": lease.ID, "trackId": lease.TrackID,
		"holderParticipantId": lease.HolderMemberID,
		"expiresAtMs":         lease.ExpiresAt.UnixMilli(),
	})
	connection.server.Rooms.Publish(connection.projectID, uuid.Nil,
		collab.RoomMessage{Data: collaborationEnvelope("lease.granted", payload,
			uuid.Nil, nil, 0)})
	return nil
}

func (connection *collaborationRoomConnection) handleHandoff(ctx context.Context,
	envelope collaborationClientEnvelope) error {
	var request collaborationHandoffRequest
	if err := decodeCollaborationJSON(envelope.Payload, &request); err != nil {
		return err
	}
	target, err := uuid.Parse(request.ParticipantID)
	if err != nil || target == uuid.Nil {
		return errors.New("invalid handoff participant")
	}
	state, err := connection.server.Collab.HandoffHost(ctx,
		connection.projectID, connection.sessionID, connection.userID,
		connection.deviceID, connection.authSessionID, target)
	if err != nil {
		code, message, retryable := collaborationRejection(err)
		connection.reject(ctx, envelope.MessageID, code, message, retryable)
		return nil
	}
	if state.Session.HostMemberID == nil {
		return errors.New("handoff produced no host")
	}
	payload, _ := json.Marshal(map[string]any{
		"hostParticipantId": *state.Session.HostMemberID, "reason": "manual",
	})
	connection.server.Rooms.Publish(connection.projectID, uuid.Nil,
		collab.RoomMessage{Data: collaborationEnvelope("session.host_changed",
			payload, uuid.Nil, nil, 0)})
	if round, roundErr := connection.server.prepareHashRound(ctx,
		connection.projectID, state); roundErr == nil {
		connection.server.publishHashRound(connection.projectID, round)
	}
	return nil
}

func (connection *collaborationRoomConnection) relayEphemeral(
	envelope collaborationClientEnvelope, interval time.Duration) error {
	connection.rateMu.Lock()
	last := connection.lastEphemeral[envelope.Type]
	now := time.Now()
	if !last.IsZero() && now.Sub(last) < interval {
		connection.rateMu.Unlock()
		return nil
	}
	connection.lastEphemeral[envelope.Type] = now
	connection.rateMu.Unlock()
	data := collaborationEnvelope(envelope.Type, envelope.Payload,
		connection.participantID, envelope.EphemeralSeq, envelope.SentAtMS)
	connection.server.Rooms.Publish(connection.projectID,
		connection.participantID, collab.RoomMessage{
			Data: data, Ephemeral: true,
			EphemeralKey: envelope.Type + ":" + connection.participantID.String(),
		})
	return nil
}

func (connection *collaborationRoomConnection) relayDurable(
	envelope collaborationClientEnvelope) error {
	data := collaborationEnvelope(envelope.Type, envelope.Payload,
		connection.participantID, nil, envelope.SentAtMS)
	connection.server.Rooms.Publish(connection.projectID,
		connection.participantID, collab.RoomMessage{Data: data})
	return nil
}

func (connection *collaborationRoomConnection) reject(ctx context.Context,
	requestMessageID, code, message string, retryable bool) {
	connection.rejectWithOperation(ctx, requestMessageID, "", code, message, retryable)
}

func (connection *collaborationRoomConnection) rejectWithOperation(
	ctx context.Context, requestMessageID, operationID, code, message string,
	retryable bool) {
	payload := map[string]any{
		"requestMessageId": requestMessageID, "code": code,
		"message": message, "retryable": retryable,
	}
	if operationID != "" {
		payload["opId"] = operationID
	}
	if project, err := connection.server.Collab.GetProject(ctx,
		connection.projectID, connection.userID); err == nil {
		payload["headSeq"] = project.Project.HeadSeq
	}
	body, _ := json.Marshal(payload)
	connection.subscription.Deliver(collab.RoomMessage{Data: collaborationEnvelope("op.rejected", body, uuid.Nil, nil, 0)})
}

func (s *Server) collaborationParticipantViews(ctx context.Context,
	projectID uuid.UUID, state collab.SessionState, currentUserID,
	currentDeviceID uuid.UUID) ([]collaborationParticipant,
	collaborationParticipant, string, error) {
	userIDs := make([]uuid.UUID, 0, len(state.Members))
	for _, member := range state.Members {
		userIDs = append(userIDs, member.UserID)
	}
	var users []model.User
	if err := s.DB.WithContext(ctx).Select("id", "nickname").
		Where("id IN ?", userIDs).Find(&users).Error; err != nil {
		return nil, collaborationParticipant{}, "", err
	}
	nicknames := make(map[uuid.UUID]string, len(users))
	for _, user := range users {
		nicknames[user.ID] = safeCollaborationNickname(user.Nickname)
	}
	var project model.CloudProject
	if err := s.DB.WithContext(ctx).Select("id", "owner_user_id").
		First(&project, "id = ?", projectID).Error; err != nil {
		return nil, collaborationParticipant{}, "", err
	}
	var memberships []model.ProjectMember
	if err := s.DB.WithContext(ctx).Where("project_id = ?", projectID).
		Find(&memberships).Error; err != nil {
		return nil, collaborationParticipant{}, "", err
	}
	roles := make(map[uuid.UUID]string, len(memberships)+1)
	colors := make(map[uuid.UUID]int16, len(memberships)+1)
	roles[project.OwnerUserID] = model.ProjectRoleOwner
	for _, membership := range memberships {
		roles[membership.UserID] = membership.Role
		colors[membership.UserID] = membership.ColorIndex
	}
	participants := make([]collaborationParticipant, 0, len(state.Members))
	var current collaborationParticipant
	currentRole := ""
	for _, member := range state.Members {
		participant := collaborationParticipant{
			ParticipantID: member.ID.String(), UserID: member.UserID.String(),
			Nickname: nicknames[member.UserID], Role: roles[member.UserID],
			Color: collaborationColor(colors[member.UserID]),
			Host: state.Session.HostMemberID != nil &&
				*state.Session.HostMemberID == member.ID,
			JoinedAtMS: member.JoinedAt.UnixMilli(),
		}
		if participant.Nickname == "" || !collab.RoleAllows(participant.Role,
			collab.PermissionView) {
			return nil, collaborationParticipant{}, "",
				errors.New("invalid participant identity")
		}
		participants = append(participants, participant)
		if member.UserID == currentUserID && member.DeviceID == currentDeviceID {
			current = participant
			currentRole = participant.Role
		}
	}
	if current.ParticipantID == "" {
		return nil, collaborationParticipant{}, "",
			errors.New("joined participant not found")
	}
	sort.Slice(participants, func(left, right int) bool {
		if participants[left].JoinedAtMS == participants[right].JoinedAtMS {
			return participants[left].ParticipantID < participants[right].ParticipantID
		}
		return participants[left].JoinedAtMS < participants[right].JoinedAtMS
	})
	return participants, current, currentRole, nil
}

func parseCollaborationWireCommand(raw json.RawMessage) (
	collaborationWireCommand, *uuid.UUID, error) {
	var keys map[string]json.RawMessage
	if err := decodeCollaborationJSON(raw, &keys); err != nil || len(keys) != 8 {
		return collaborationWireCommand{}, nil,
			errors.New("command must contain exactly eight fields")
	}
	for _, key := range []string{"schemaVersion", "opId", "transactionId",
		"baseServerSeq", "kind", "payload", "preconditions", "touchedFields"} {
		if _, exists := keys[key]; !exists {
			return collaborationWireCommand{}, nil,
				errors.New("command field is missing")
		}
	}
	var command collaborationWireCommand
	if err := decodeCollaborationJSON(raw, &command); err != nil {
		return collaborationWireCommand{}, nil, err
	}
	var transactionID *uuid.UUID
	if !bytes.Equal(bytes.TrimSpace(command.TransactionID), []byte("null")) {
		var transactionText string
		if err := json.Unmarshal(command.TransactionID, &transactionText); err != nil {
			return collaborationWireCommand{}, nil, errors.New("invalid transaction id")
		}
		parsed, err := uuid.Parse(transactionText)
		if err != nil || parsed == uuid.Nil {
			return collaborationWireCommand{}, nil, errors.New("invalid transaction id")
		}
		transactionID = &parsed
	}
	return command, transactionID, nil
}

func validateCollaborationEnvelope(envelope collaborationClientEnvelope,
	expectedType string, ephemeral bool) error {
	if envelope.Protocol != collab.CollaborationProtocol ||
		(expectedType != "" && envelope.Type != expectedType) ||
		envelope.Type == "" || len(envelope.Type) > 64 ||
		uuid.Validate(envelope.MessageID) != nil || envelope.SentAtMS < 0 ||
		len(envelope.Payload) == 0 {
		return errors.New("invalid collaboration envelope")
	}
	if envelope.ParticipantID != "" {
		return errors.New("client cannot select participant identity")
	}
	if ephemeral != (envelope.EphemeralSeq != nil) {
		return errors.New("ephemeral sequence mismatch")
	}
	return nil
}

func validateCollaborationHello(hello collaborationHello) error {
	if hello.AppVersion == "" || len(hello.AppVersion) > 64 ||
		hello.EngineVersion == "" || len(hello.EngineVersion) > 64 ||
		hello.CommandSchemaVersion != collab.CollaborationCommandSchemaVersion ||
		hello.ProjectFormatVersion != collab.CollaborationProjectFormatVersion ||
		hello.AfterServerSeq < 0 {
		return errors.New("unsupported collaboration client")
	}
	if hello.StateHash != nil && !validSHA256(*hello.StateHash) {
		return errors.New("invalid state hash")
	}
	return nil
}

func validateCollaborationPresence(kind string, raw json.RawMessage) error {
	var payload collaborationSafePresence
	if err := decodeCollaborationJSON(raw, &payload); err != nil {
		return err
	}
	allowedSurfaces := map[string]bool{
		"timeline": true, "track_list": true, "transport": true,
		"mixer": true, "piano_roll": true, "automation_editor": true,
		"sample_editor": true, "builtin_plugin": true, "toolbar": true,
		"settings": true, "ai": true, "browser": true,
		"file_browser": true, "hidden": true,
	}
	if !allowedSurfaces[payload.Surface] ||
		(payload.Precision != "exact" && payload.Precision != "coarse" &&
			payload.Precision != "hidden") {
		return errors.New("invalid presence surface")
	}
	sensitive := payload.Surface == "settings" || payload.Surface == "ai" ||
		payload.Surface == "browser" || payload.Surface == "file_browser"
	if sensitive && payload.Precision == "exact" {
		return errors.New("sensitive surface cannot be exact")
	}
	if payload.Precision == "hidden" && payload.Surface != "hidden" {
		return errors.New("hidden precision requires hidden surface")
	}
	hasDetail := payload.TargetID != "" || payload.TrackID != "" ||
		payload.ClipID != "" || payload.Lane != "" || payload.ControlID != "" ||
		payload.TimeSeconds != nil || payload.Beat != nil || payload.Pitch != nil ||
		payload.LaneFraction != nil || payload.U != nil || payload.V != nil ||
		len(payload.EntityIDs) != 0 || payload.Gesture != "" ||
		payload.Phase != "" || payload.Button != ""
	if payload.Precision != "exact" && hasDetail {
		return errors.New("coarse presence cannot contain detail")
	}
	for _, value := range []struct {
		text string
		max  int
	}{{payload.TargetID, 96}, {payload.Lane, 64}, {payload.ControlID, 96}} {
		if value.text != "" && (len(value.text) > value.max ||
			!collaborationSemanticID.MatchString(value.text)) {
			return errors.New("invalid presence semantic id")
		}
	}
	for _, value := range []string{payload.TrackID, payload.ClipID} {
		if value != "" && uuid.Validate(value) != nil {
			return errors.New("invalid presence entity id")
		}
	}
	if payload.TimeSeconds != nil && *payload.TimeSeconds < 0 ||
		payload.Pitch != nil && (*payload.Pitch < 0 || *payload.Pitch > 127) ||
		!unitInterval(payload.LaneFraction) || !unitInterval(payload.U) ||
		!unitInterval(payload.V) {
		return errors.New("invalid presence coordinate")
	}
	if len(payload.EntityIDs) > 256 {
		return errors.New("too many presence entities")
	}
	seen := make(map[string]bool, len(payload.EntityIDs))
	for _, entityID := range payload.EntityIDs {
		if len(entityID) > 64 || !collaborationSemanticID.MatchString(entityID) ||
			seen[entityID] {
			return errors.New("invalid presence entity list")
		}
		seen[entityID] = true
	}
	switch kind {
	case "presence.cursor":
		if payload.Phase != "" || payload.Button != "" ||
			payload.Gesture != "" || len(payload.EntityIDs) != 0 {
			return errors.New("cursor payload contains another gesture")
		}
	case "presence.click":
		if (payload.Phase != "press" && payload.Phase != "release") ||
			(payload.Button != "primary" && payload.Button != "secondary" &&
				payload.Button != "middle") {
			return errors.New("invalid click")
		}
	case "presence.selection":
		if payload.Phase != "" || payload.Button != "" || payload.Gesture != "" {
			return errors.New("invalid selection")
		}
	case "presence.drag":
		if payload.Gesture == "" || len(payload.Gesture) > 48 ||
			!collaborationSemanticID.MatchString(payload.Gesture) {
			return errors.New("invalid drag")
		}
	}
	return nil
}

func decodeCollaborationJSON(data []byte, target any) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return err
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		return errors.New("collaboration JSON contains trailing data")
	}
	return nil
}

func collaborationEnvelope(kind string, payload json.RawMessage,
	participantID uuid.UUID, ephemeralSequence *uint64, sentAtMS int64) []byte {
	envelope := collaborationServerEnvelope{
		Protocol: collab.CollaborationProtocol, Type: kind,
		MessageID: uuid.NewString(), EphemeralSeq: ephemeralSequence,
		SentAtMS: sentAtMS, ServerTimeMS: time.Now().UnixMilli(), Payload: payload,
	}
	if participantID != uuid.Nil {
		envelope.ParticipantID = participantID.String()
	}
	body, _ := json.Marshal(envelope)
	return body
}

func collaborationCommittedEnvelope(operation model.ProjectOperation) []byte {
	var preconditions any = []any{}
	var touchedFields any = []any{}
	_ = json.Unmarshal(operation.Preconditions, &preconditions)
	_ = json.Unmarshal(operation.TouchedFields, &touchedFields)
	command := map[string]any{
		"schemaVersion": operation.SchemaVersion, "opId": operation.OpID,
		"transactionId": operation.TransactionID,
		"baseServerSeq": operation.BaseSeq, "kind": operation.Kind,
		"payload":       json.RawMessage(operation.Payload),
		"preconditions": preconditions, "touchedFields": touchedFields,
	}
	payload, _ := json.Marshal(map[string]any{
		"projectId": operation.ProjectID, "serverSeq": operation.Seq,
		"actorUserId":   operation.ActorUserID,
		"actorDeviceId": operation.ActorDeviceID,
		"committedAtMs": operation.CreatedAt.UnixMilli(), "command": command,
	})
	return collaborationEnvelope("op.committed", payload, uuid.Nil, nil, 0)
}

func collaborationRejection(err error) (string, string, bool) {
	switch {
	case errors.Is(err, collab.ErrHashConsensusBlocked):
		return "hash_consensus_required", "Complete the current state hash round before editing.", true
	case errors.Is(err, collab.ErrCloudRecordingDisabled):
		return "cloud_recording_disabled", "Recording is not available in cloud projects.", false
	case errors.Is(err, collab.ErrValidation):
		return "invalid_message", "The operation is invalid.", false
	case errors.Is(err, collab.ErrOperationIDReuse):
		return "operation_id_reused", "Operation identifier was reused with different content.", false
	case errors.Is(err, collab.ErrForbidden):
		return "forbidden", "This role cannot perform the operation.", false
	case errors.Is(err, collab.ErrProjectInactive),
		errors.Is(err, collab.ErrLiveSessionRequired),
		errors.Is(err, collab.ErrSessionEnded):
		return "session_inactive", "The live session is not active.", true
	case errors.Is(err, collab.ErrBaseSeqAhead), errors.Is(err, collab.ErrBaseSeqMismatch):
		return "sequence_gap", "Project resynchronization is required.", true
	case errors.Is(err, collab.ErrEntityDeleted):
		return "entity_deleted", "The target was deleted.", false
	case errors.Is(err, collab.ErrAssetUnavailable):
		return "asset_incomplete", "Complete and verify every referenced asset before committing the operation.", false
	case errors.Is(err, collab.ErrLeaseHeld), errors.Is(err, collab.ErrLeaseExpired),
		errors.Is(err, collab.ErrLeaseRequired):
		return "lease_conflict", "The recording lease is unavailable.", true
	case errors.Is(err, collab.ErrConflict):
		return "stale_precondition", "A newer operation changed this field.", false
	default:
		return "conflict", "The operation could not be committed.", true
	}
}

func collaborationWSDBTimeout(seconds int) time.Duration {
	if seconds < 3 || seconds > 60 {
		seconds = 15
	}
	return time.Duration(seconds) * time.Second
}

func normalizedRoomQueueBytes(configured int64) int64 {
	if configured < 2<<20 || configured > collab.MaximumRoomQueueBytes {
		return collab.DefaultRoomQueueBytes
	}
	return configured
}

func writeCollaborationSocket(connection *websocket.Conn, data []byte) error {
	ctx, cancel := context.WithTimeout(context.Background(),
		collaborationWriteTimeout)
	defer cancel()
	return connection.Write(ctx, websocket.MessageText, data)
}

func isCollaborationEphemeral(kind string) bool {
	return strings.HasPrefix(kind, "presence.") ||
		kind == "transport.state" || kind == "transport.follow"
}

func ephemeralInterval(kind string) time.Duration {
	if kind == "presence.cursor" || kind == "presence.drag" {
		return 50 * time.Millisecond
	}
	return 25 * time.Millisecond
}

func validateOptionalUUIDJSON(raw json.RawMessage, required bool) error {
	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) == 0 || bytes.Equal(trimmed, []byte("null")) {
		if required {
			return errors.New("target participant is required")
		}
		return nil
	}
	var value string
	if json.Unmarshal(trimmed, &value) != nil || uuid.Validate(value) != nil {
		return errors.New("invalid target participant")
	}
	return nil
}

func validSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, character := range value {
		if character < '0' || character > '9' {
			if character < 'a' || character > 'f' {
				return false
			}
		}
	}
	return true
}

func unitInterval(value *float64) bool {
	return value == nil || *value >= 0 && *value <= 1
}

func safeCollaborationNickname(value string) string {
	value = strings.TrimSpace(value)
	result := make([]rune, 0, 80)
	for _, character := range value {
		if len(result) == 80 {
			break
		}
		if !unicode.IsControl(character) {
			result = append(result, character)
		}
	}
	if len(result) == 0 {
		return "Participant"
	}
	return string(result)
}

func collaborationColor(index int16) string {
	palette := [...]string{
		"#4F8CFF", "#F06292", "#66BB6A", "#FFB74D",
		"#AB47BC", "#26C6DA", "#EF5350", "#9CCC65",
	}
	if index < 0 {
		index = 0
	}
	return palette[int(index)%len(palette)]
}

func sameOptionalUUID(left, right *uuid.UUID) bool {
	if left == nil || right == nil {
		return left == nil && right == nil
	}
	return *left == *right
}

func safeWebSocketReason(value string) string {
	value = strings.TrimSpace(value)
	if value == "" {
		return "connection closed"
	}
	if len(value) > 96 {
		return value[:96]
	}
	return value
}
