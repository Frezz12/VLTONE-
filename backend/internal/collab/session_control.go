package collab

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"sort"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"vltstudio/backend/internal/model"
)

// Transport commands are durable control state. Cursor/position telemetry is
// intentionally not part of this stream or the musical project operation log.
type TransportControl struct {
	Revision            int64   `json:"revision"`
	Playing             bool    `json:"playing"`
	PositionSeconds     float64 `json:"positionSeconds"`
	Rate                float64 `json:"rate"`
	ServerTimeMS        int64   `json:"serverTimeMs"`
	EffectiveAtServerMS int64   `json:"effectiveAtServerMs"`
	LoopEnabled         bool    `json:"loopEnabled"`
	LoopStartSeconds    float64 `json:"loopStartSeconds"`
	LoopEndSeconds      float64 `json:"loopEndSeconds"`
}

type AuditionControl struct {
	Revision      int64       `json:"revision"`
	MutedTrackIDs []uuid.UUID `json:"mutedTrackIds"`
	SoloTrackIDs  []uuid.UUID `json:"soloTrackIds"`
}

type SessionControl struct {
	Mode           string           `json:"mode"`
	SessionVersion int64            `json:"sessionVersion"`
	HostMemberID   *uuid.UUID       `json:"hostMemberId"`
	Transport      TransportControl `json:"transport"`
	Audition       AuditionControl  `json:"audition"`
}

type ControlAction struct {
	ActionID               uuid.UUID  `json:"actionId"`
	ExpectedSessionVersion int64      `json:"expectedSessionVersion"`
	Kind                   string     `json:"kind"`
	PositionSeconds        *float64   `json:"positionSeconds,omitempty"`
	Rate                   *float64   `json:"rate,omitempty"`
	TrackID                *uuid.UUID `json:"trackId,omitempty"`
	Muted                  *bool      `json:"muted,omitempty"`
	Solo                   *bool      `json:"solo,omitempty"`
	LoopEnabled            *bool      `json:"loopEnabled,omitempty"`
	LoopStartSeconds       *float64   `json:"loopStartSeconds,omitempty"`
	LoopEndSeconds         *float64   `json:"loopEndSeconds,omitempty"`
}

type ControlResult struct {
	ActionID uuid.UUID      `json:"actionId"`
	Control  SessionControl `json:"control"`
}

type sessionControlReceipt struct {
	SessionID     uuid.UUID `gorm:"type:uuid;primaryKey"`
	ActionID      uuid.UUID `gorm:"type:uuid;primaryKey"`
	ActorMemberID uuid.UUID `gorm:"type:uuid"`
	RequestHash   string
	Result        datatypes.JSON `gorm:"type:jsonb"`
	CreatedAt     time.Time
}

func (sessionControlReceipt) TableName() string { return "project_session_controls" }

func ControlSnapshot(session model.ProjectSession) SessionControl {
	result := SessionControl{Mode: session.Mode, SessionVersion: session.Version,
		HostMemberID: session.HostMemberID, Transport: TransportControl{Rate: 1},
		Audition: AuditionControl{MutedTrackIDs: []uuid.UUID{}, SoloTrackIDs: []uuid.UUID{}}}
	_ = json.Unmarshal(session.TransportState, &result.Transport)
	_ = json.Unmarshal(session.AuditionState, &result.Audition)
	if result.Transport.Rate == 0 {
		result.Transport.Rate = 1
	}
	if result.Audition.MutedTrackIDs == nil {
		result.Audition.MutedTrackIDs = []uuid.UUID{}
	}
	if result.Audition.SoloTrackIDs == nil {
		result.Audition.SoloTrackIDs = []uuid.UUID{}
	}
	return result
}

// SessionAllowsEdit is shared by commands, recording leases and asset writes.
func SessionAllowsEdit(session model.ProjectSession, member model.ProjectSessionMember) error {
	if session.CommandSchemaVersion >= CollaborationCommandSchemaV3 &&
		(member.ReadinessStatus != model.SessionReadinessReady ||
			member.ReadinessRevision != session.PluginRequirementsRevision ||
			!RoleAllows(member.EffectiveRole, PermissionEdit)) {
		return ErrPluginNotReady
	}
	if session.CommandSchemaVersion >= CollaborationCommandSchemaV6 &&
		session.Mode == model.SessionModeFollowHost &&
		(session.HostMemberID == nil || *session.HostMemberID != member.ID) {
		return ErrForbidden
	}
	return nil
}

func validControlNumber(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0) && value >= 0 && value <= 1e9
}

func validateTransport(state TransportControl) error {
	if !validControlNumber(state.PositionSeconds) || !validControlNumber(state.LoopStartSeconds) ||
		!validControlNumber(state.LoopEndSeconds) || math.IsNaN(state.Rate) || math.IsInf(state.Rate, 0) || state.Rate < .25 || state.Rate > 4 ||
		(state.LoopEnabled && state.LoopEndSeconds <= state.LoopStartSeconds) {
		return invalidf("invalid shared transport state")
	}
	return nil
}

func normalizeAudition(state *AuditionControl) error {
	for _, ids := range []*[]uuid.UUID{&state.MutedTrackIDs, &state.SoloTrackIDs} {
		if len(*ids) > 8192 {
			return invalidf("too many audition tracks")
		}
		seen := map[uuid.UUID]bool{}
		result := make([]uuid.UUID, 0, len(*ids))
		for _, id := range *ids {
			if id == uuid.Nil {
				return invalidf("invalid audition track")
			}
			if !seen[id] {
				result = append(result, id)
				seen[id] = true
			}
		}
		sort.Slice(result, func(i, j int) bool { return result[i].String() < result[j].String() })
		*ids = result
	}
	return nil
}

func auditionSet(ids []uuid.UUID, id uuid.UUID, enabled bool) []uuid.UUID {
	result := make([]uuid.UUID, 0, len(ids)+1)
	for _, old := range ids {
		if old != id {
			result = append(result, old)
		}
	}
	if enabled {
		result = append(result, id)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].String() < result[j].String() })
	return result
}

func advanceTransport(state *TransportControl, now int64) {
	if state.Playing && state.ServerTimeMS > 0 && now > state.ServerTimeMS {
		state.PositionSeconds += float64(now-state.ServerTimeMS) / 1000 * state.Rate
		if state.LoopEnabled && state.LoopEndSeconds > state.LoopStartSeconds && state.PositionSeconds >= state.LoopEndSeconds {
			state.PositionSeconds = state.LoopStartSeconds + math.Mod(state.PositionSeconds-state.LoopStartSeconds, state.LoopEndSeconds-state.LoopStartSeconds)
		}
	}
	state.ServerTimeMS, state.EffectiveAtServerMS = now, now
}

func applyControl(control *SessionControl, action ControlAction, now int64) error {
	if action.ActionID == uuid.Nil || action.ExpectedSessionVersion <= 0 {
		return invalidf("control action identity and session version required")
	}
	if action.Kind == "audition" {
		if action.TrackID == nil || *action.TrackID == uuid.Nil || (action.Muted == nil && action.Solo == nil) || action.PositionSeconds != nil || action.Rate != nil || action.LoopEnabled != nil || action.LoopStartSeconds != nil || action.LoopEndSeconds != nil {
			return invalidf("invalid audition action")
		}
		if action.Muted != nil {
			control.Audition.MutedTrackIDs = auditionSet(control.Audition.MutedTrackIDs, *action.TrackID, *action.Muted)
		}
		if action.Solo != nil {
			control.Audition.SoloTrackIDs = auditionSet(control.Audition.SoloTrackIDs, *action.TrackID, *action.Solo)
		}
		control.Audition.Revision++
		return normalizeAudition(&control.Audition)
	}
	if action.TrackID != nil || action.Muted != nil || action.Solo != nil {
		return invalidf("transport action contains audition fields")
	}
	if action.Kind != "loop" && (action.LoopEnabled != nil || action.LoopStartSeconds != nil || action.LoopEndSeconds != nil) {
		return invalidf("only loop action may update loop range")
	}
	advanceTransport(&control.Transport, now)
	if action.PositionSeconds != nil {
		control.Transport.PositionSeconds = *action.PositionSeconds
	}
	if action.Rate != nil {
		control.Transport.Rate = *action.Rate
	}
	switch action.Kind {
	case "play":
		control.Transport.Playing = true
	case "pause":
		control.Transport.Playing = false
	case "stop":
		control.Transport.Playing = false
	case "seek":
		if action.PositionSeconds == nil {
			return invalidf("seek position required")
		}
	case "loop":
		if action.LoopEnabled == nil || action.LoopStartSeconds == nil || action.LoopEndSeconds == nil {
			return invalidf("complete loop range required")
		}
		control.Transport.LoopEnabled, control.Transport.LoopStartSeconds, control.Transport.LoopEndSeconds = *action.LoopEnabled, *action.LoopStartSeconds, *action.LoopEndSeconds
	default:
		return invalidf("unsupported session control")
	}
	if err := validateTransport(control.Transport); err != nil {
		return err
	}
	control.Transport.Revision++
	return nil
}

func (s *Store) ApplySessionControl(ctx context.Context, projectID, sessionID, userID, deviceID, desktopID uuid.UUID, action ControlAction) (ControlResult, bool, error) {
	var result ControlResult
	duplicate := false
	request, err := json.Marshal(action)
	if err != nil {
		return result, false, invalidf("invalid control")
	}
	hash := sha256.Sum256(request)
	requestHash := hex.EncodeToString(hash[:])
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, userID, deviceID, desktopID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, userID, true)
		if err != nil {
			return err
		}
		if view.Project.Status != model.ProjectActive || !RoleAllows(view.Role, PermissionEdit) {
			return ErrForbidden
		}
		session, err := s.liveSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		member, err := s.activeSessionMemberTx(tx, sessionID, userID, deviceID, desktopID, true)
		if err != nil {
			return err
		}
		if session.CommandSchemaVersion < CollaborationCommandSchemaV6 {
			return ErrVersionMismatch
		}
		var receipt sessionControlReceipt
		lookup := tx.First(&receipt, "session_id = ? AND action_id = ?", sessionID, action.ActionID)
		if lookup.Error == nil {
			if receipt.ActorMemberID != member.ID || receipt.RequestHash != requestHash {
				return ErrOperationIDReuse
			}
			duplicate = true
			// Return the current snapshot, not an obsolete transport state. The
			// original action is acknowledged without applying it a second time.
			result = ControlResult{ActionID: action.ActionID, Control: ControlSnapshot(session)}
			return nil
		}
		if !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
			return lookup.Error
		}
		if err := SessionAllowsEdit(session, member); err != nil {
			return err
		}
		if session.Mode == model.SessionModeIndependent {
			return ErrForbidden
		}
		if session.Version != action.ExpectedSessionVersion {
			return ErrSessionVersion
		}
		control := ControlSnapshot(session)
		if err := applyControl(&control, action, s.now().UnixMilli()); err != nil {
			return err
		}
		transport, _ := json.Marshal(control.Transport)
		audition, _ := json.Marshal(control.Audition)
		if err := tx.Model(&session).Updates(map[string]any{"transport_state": datatypes.JSON(transport), "audition_state": datatypes.JSON(audition), "updated_at": s.now()}).Error; err != nil {
			return err
		}
		result = ControlResult{ActionID: action.ActionID, Control: control}
		encoded, _ := json.Marshal(result)
		return tx.Create(&sessionControlReceipt{SessionID: sessionID, ActionID: action.ActionID, ActorMemberID: member.ID, RequestHash: requestHash, Result: datatypes.JSON(encoded), CreatedAt: s.now()}).Error
	})
	return result, duplicate, err
}

func (s *Store) ChangeSessionMode(ctx context.Context, projectID, sessionID, userID, deviceID, desktopID uuid.UUID, mode string, expectedVersion int64, transport *TransportControl, audition *AuditionControl) (SessionState, error) {
	var state SessionState
	if !ValidSessionMode(mode) {
		return state, invalidf("invalid session mode")
	}
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, userID, deviceID, desktopID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, userID, true)
		if err != nil {
			return err
		}
		session, err := s.liveSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		member, err := s.activeSessionMemberTx(tx, sessionID, userID, deviceID, desktopID, true)
		if err != nil {
			return err
		}
		if session.CommandSchemaVersion < CollaborationCommandSchemaV6 {
			return ErrVersionMismatch
		}
		if view.Role != model.ProjectRoleOwner && (session.HostMemberID == nil || *session.HostMemberID != member.ID) {
			return ErrForbidden
		}
		if !RoleAllows(member.EffectiveRole, PermissionEdit) || member.ReadinessStatus != model.SessionReadinessReady || member.ReadinessRevision != session.PluginRequirementsRevision {
			return ErrPluginNotReady
		}
		if session.Version != expectedVersion {
			return ErrSessionVersion
		}
		if mode == session.Mode {
			state, err = s.sessionStateTx(tx, session)
			return err
		}
		var recordings int64
		if err := tx.Model(&model.ProjectTrackLease{}).Where("session_id = ? AND expires_at > ?", sessionID, s.now()).Count(&recordings).Error; err != nil {
			return err
		}
		if recordings > 0 {
			return ErrRecordingActive
		}
		// Append-only capture can create independent clips on the same track,
		// so it holds a renewable participant marker instead of a track lock.
		if err := tx.Model(&editLeaseRow{}).Where("session_id = ? AND expires_at > ? AND jsonb_exists(field_keys, 'recording:' || holder_member_id::text)", sessionID, s.now()).Count(&recordings).Error; err != nil {
			return err
		}
		if recordings > 0 {
			return ErrRecordingActive
		}
		if mode == model.SessionModeFollowHost && session.HostMemberID == nil {
			return ErrForbidden
		}
		control := ControlSnapshot(session)
		advanceTransport(&control.Transport, s.now().UnixMilli())
		if mode != model.SessionModeIndependent && session.Mode == model.SessionModeIndependent {
			// Seed only from the actual leader. An owner who has delegated the
			// leader role must first take it back before supplying local state.
			if session.HostMemberID == nil || *session.HostMemberID != member.ID {
				return ErrForbidden
			}
			if transport == nil || audition == nil {
				return invalidf("leader transport and audition required when enabling shared playback")
			}
			if err := validateTransport(*transport); err != nil {
				return err
			}
			if err := normalizeAudition(audition); err != nil {
				return err
			}
			control.Transport = *transport
			control.Audition = *audition
		}
		if mode == model.SessionModeIndependent {
			control.Transport.Playing = false
		}
		control.Transport.Revision = ControlSnapshot(session).Transport.Revision + 1
		control.Audition.Revision = ControlSnapshot(session).Audition.Revision + 1
		control.Transport.ServerTimeMS, control.Transport.EffectiveAtServerMS = s.now().UnixMilli(), s.now().UnixMilli()
		encodedTransport, _ := json.Marshal(control.Transport)
		encodedAudition, _ := json.Marshal(control.Audition)
		if err := tx.Model(&session).Updates(map[string]any{"mode": mode, "version": gorm.Expr("version + 1"), "transport_state": datatypes.JSON(encodedTransport), "audition_state": datatypes.JSON(encodedAudition), "updated_at": s.now()}).Error; err != nil {
			return err
		}
		session.Mode = mode
		session.Version++
		session.TransportState = datatypes.JSON(encodedTransport)
		session.AuditionState = datatypes.JSON(encodedAudition)
		// Recording cannot continue under a policy which revoked its holder.
		if mode == model.SessionModeFollowHost {
			if err := tx.Where("session_id = ? AND holder_member_id <> ?", sessionID, *session.HostMemberID).Delete(&model.ProjectTrackLease{}).Error; err != nil {
				return err
			}
		}
		state, err = s.sessionStateTx(tx, session)
		return err
	})
	return state, err
}

func (s *Store) pausePresenterTx(tx *gorm.DB, session *model.ProjectSession, now time.Time) error {
	return s.pausePresenterWithAssignmentTx(tx, session, now, false)
}

func (s *Store) pausePresenterWithAssignmentTx(tx *gorm.DB, session *model.ProjectSession, now time.Time, retainAssignment bool) error {
	control := ControlSnapshot(*session)
	advanceTransport(&control.Transport, now.UnixMilli())
	control.Transport.Playing = false
	control.Transport.Revision++
	encoded, _ := json.Marshal(control.Transport)
	updates := map[string]any{"transport_state": datatypes.JSON(encoded), "version": gorm.Expr("version + 1"), "updated_at": now}
	if !retainAssignment {
		updates["host_member_id"] = nil
	}
	if err := tx.Model(session).Updates(updates).Error; err != nil {
		return err
	}
	if !retainAssignment {
		session.HostMemberID = nil
	}
	session.TransportState = datatypes.JSON(encoded)
	session.Version++
	session.UpdatedAt = now
	return nil
}

func (s *Store) PauseDisconnectedPresenter(ctx context.Context, projectID, sessionID, memberID uuid.UUID) (SessionState, bool, error) {
	var state SessionState
	changed := false
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		var project model.CloudProject
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&project, "id = ?", projectID).Error; err != nil {
			return err
		}
		session, err := s.openSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		if session.CommandSchemaVersion < CollaborationCommandSchemaV6 || session.Mode != model.SessionModeFollowHost || session.HostMemberID == nil || *session.HostMemberID != memberID {
			return nil
		}
		if err := s.pausePresenterWithAssignmentTx(tx, &session, s.now(), true); err != nil {
			return err
		}
		changed = true
		state, err = s.sessionStateTx(tx, session)
		return err
	})
	return state, changed, err
}

type sessionExclusion struct {
	SessionID uuid.UUID `gorm:"type:uuid;primaryKey"`
	UserID    uuid.UUID `gorm:"type:uuid;primaryKey"`
	CreatedBy uuid.UUID
	CreatedAt time.Time
}

func (sessionExclusion) TableName() string { return "project_session_exclusions" }

type projectBan struct {
	ProjectID uuid.UUID `gorm:"type:uuid;primaryKey"`
	UserID    uuid.UUID `gorm:"type:uuid;primaryKey"`
	CreatedBy uuid.UUID
	CreatedAt time.Time
}

func (projectBan) TableName() string { return "project_bans" }

func requireNotBannedTx(tx *gorm.DB, projectID, userID uuid.UUID) error {
	var count int64
	if err := tx.Model(&projectBan{}).Where("project_id = ? AND user_id = ?", projectID, userID).Count(&count).Error; err != nil {
		return err
	}
	if count != 0 {
		return ErrProjectBanned
	}
	return nil
}

func requireNotExcludedTx(tx *gorm.DB, sessionID, userID uuid.UUID) error {
	var count int64
	if err := tx.Model(&sessionExclusion{}).Where("session_id = ? AND user_id = ?", sessionID, userID).Count(&count).Error; err != nil {
		return err
	}
	if count != 0 {
		return ErrSessionExcluded
	}
	return nil
}

func (s *Store) ModerateSession(ctx context.Context, projectID, sessionID, actorID, targetID uuid.UUID, action string) (SessionState, error) {
	var state SessionState
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		view, err := s.projectAccess(tx, projectID, actorID, true)
		if err != nil {
			return err
		}
		if view.Role != model.ProjectRoleOwner || targetID == uuid.Nil || targetID == view.Project.OwnerUserID {
			return ErrForbidden
		}
		session, err := s.openSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		now := s.now()
		switch action {
		case "kick":
			if err := tx.Clauses(clause.OnConflict{DoNothing: true}).Create(&sessionExclusion{SessionID: sessionID, UserID: targetID, CreatedBy: actorID, CreatedAt: now}).Error; err != nil {
				return err
			}
		case "ban":
			if err := tx.Clauses(clause.OnConflict{DoNothing: true}).Create(&projectBan{ProjectID: projectID, UserID: targetID, CreatedBy: actorID, CreatedAt: now}).Error; err != nil {
				return err
			}
		case "readmit":
			if err := tx.Where("session_id = ? AND user_id = ?", sessionID, targetID).Delete(&sessionExclusion{}).Error; err != nil {
				return err
			}
		case "unban":
			if err := tx.Where("project_id = ? AND user_id = ?", projectID, targetID).Delete(&projectBan{}).Error; err != nil {
				return err
			}
		default:
			return invalidf("invalid moderation action")
		}
		if action == "kick" || action == "ban" {
			var members []model.ProjectSessionMember
			if err := tx.Where("session_id = ? AND user_id = ? AND left_at IS NULL", sessionID, targetID).Find(&members).Error; err != nil {
				return err
			}
			for _, member := range members {
				if err := s.leaveMemberTx(tx, view.Project, &session, member, now); err != nil {
					return err
				}
			}
			// A presenter may already have timed out. Removing that user must
			// also revoke the retained assignment so readmission cannot restore it.
			if session.HostMemberID != nil {
				var host model.ProjectSessionMember
				if err := tx.First(&host, "id = ?", *session.HostMemberID).Error; err != nil {
					return err
				}
				if host.UserID == targetID {
					if err := s.pausePresenterTx(tx, &session, now); err != nil {
						return err
					}
				}
			}
		}
		state, err = s.sessionStateTx(tx, session)
		return err
	})
	return state, err
}
