package collab

import (
	"context"
	"errors"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
)

type SessionState struct {
	Session model.ProjectSession         `json:"session"`
	Members []model.ProjectSessionMember `json:"members"`
}

type EndedIdleSession struct {
	ProjectID uuid.UUID
	SessionID uuid.UUID
	FinalSeq  int64
	Finalized bool
}

type ReapedSessionMembers struct {
	ProjectID            uuid.UUID
	SessionID            uuid.UUID
	MemberIDs            []uuid.UUID
	PreviousHostMemberID *uuid.UUID
	HostMemberID         *uuid.UUID
}

const (
	MinimumMemberStaleAfter = 30 * time.Second
	MaximumMemberStaleAfter = 10 * time.Minute
	MaximumReaperBatch      = 1000
)

type LeaseHeldError struct {
	HolderMemberID uuid.UUID
	ExpiresAt      time.Time
}

func (e *LeaseHeldError) Error() string { return ErrLeaseHeld.Error() }
func (e *LeaseHeldError) Unwrap() error { return ErrLeaseHeld }

func (s *Store) StartSession(ctx context.Context, projectID, actorUserID,
	actorDeviceID, actorSessionID uuid.UUID, mode string) (SessionState, error) {
	return s.startSession(ctx, projectID, actorUserID, actorDeviceID,
		actorSessionID, mode, nil)
}

func (s *Store) StartSessionCompatible(ctx context.Context, projectID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID, mode string,
	compatibility ClientCompatibility) (SessionState, error) {
	return s.startSession(ctx, projectID, actorUserID, actorDeviceID,
		actorSessionID, mode, &compatibility)
}

func (s *Store) startSession(ctx context.Context, projectID, actorUserID,
	actorDeviceID, actorSessionID uuid.UUID, mode string,
	compatibility *ClientCompatibility) (SessionState, error) {
	if !ValidSessionMode(mode) || actorDeviceID == uuid.Nil {
		return SessionState{}, invalidf("session mode or device is invalid")
	}
	var state SessionState
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if view.Role != model.ProjectRoleOwner {
			return ErrForbidden
		}
		if compatibility != nil {
			if err := ValidateClientCompatibility(view.Project, *compatibility); err != nil {
				return err
			}
		}
		if view.Project.Status != model.ProjectActive {
			return ErrProjectInactive
		}
		var existing model.ProjectSession
		lookup := tx.Where("project_id = ? AND status IN ?", projectID,
			[]string{model.ProjectSessionStarting, model.ProjectSessionActive,
				model.ProjectSessionEnding}).First(&existing)
		if lookup.Error == nil {
			return ErrConflict
		}
		if !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
			return lookup.Error
		}
		now := s.now()
		creator := actorUserID
		session := model.ProjectSession{
			ID: uuid.New(), ProjectID: projectID, CreatedBy: &creator, Mode: mode,
			Status: model.ProjectSessionStarting, Version: 1, CreatedAt: now, UpdatedAt: now,
		}
		if err := tx.Create(&session).Error; err != nil {
			return err
		}
		member := model.ProjectSessionMember{
			ID: uuid.New(), SessionID: session.ID, UserID: actorUserID, DeviceID: actorDeviceID,
			DesktopSessionID: &actorSessionID, JoinedAt: now, LastSeenAt: now,
		}
		if err := tx.Create(&member).Error; err != nil {
			return err
		}
		startedAt := now
		hostID := member.ID
		if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).Updates(map[string]any{
			"host_member_id": hostID, "status": model.ProjectSessionActive,
			"started_at": startedAt, "updated_at": now,
		}).Error; err != nil {
			return err
		}
		session.HostMemberID = &hostID
		session.Status = model.ProjectSessionActive
		session.StartedAt = &startedAt
		state = SessionState{Session: session, Members: []model.ProjectSessionMember{member}}
		return nil
	})
	return state, err
}

func (s *Store) GetActiveSession(ctx context.Context, projectID, actorUserID uuid.UUID) (SessionState, error) {
	view, err := s.projectAccess(s.DB.WithContext(ctx), projectID, actorUserID, false)
	if err != nil {
		return SessionState{}, err
	}
	if !RoleAllows(view.Role, PermissionView) {
		return SessionState{}, ErrForbidden
	}
	var session model.ProjectSession
	if err := s.DB.WithContext(ctx).Where("project_id = ? AND status IN ?", projectID,
		[]string{model.ProjectSessionActive, model.ProjectSessionEnding}).
		First(&session).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return SessionState{}, ErrNotFound
		}
		return SessionState{}, err
	}
	return s.sessionState(ctx, session)
}

func (s *Store) JoinSession(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID) (SessionState, error) {
	return s.joinSession(ctx, projectID, sessionID, actorUserID, actorDeviceID,
		actorSessionID, nil)
}

func (s *Store) JoinSessionCompatible(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID,
	compatibility ClientCompatibility) (SessionState, error) {
	return s.joinSession(ctx, projectID, sessionID, actorUserID, actorDeviceID,
		actorSessionID, &compatibility)
}

func (s *Store) joinSession(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID,
	compatibility *ClientCompatibility) (SessionState, error) {
	if actorDeviceID == uuid.Nil {
		return SessionState{}, invalidf("device is required")
	}
	var state SessionState
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if compatibility != nil {
			if err := ValidateClientCompatibility(view.Project, *compatibility); err != nil {
				return err
			}
		}
		if !RoleAllows(view.Role, PermissionJoinSession) {
			return ErrForbidden
		}
		session, err := s.openSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		if session.Status == model.ProjectSessionEnding && !RoleAllows(view.Role, PermissionEdit) {
			return ErrForbidden
		}
		now := s.now()
		member, err := s.activeSessionMemberTx(tx, session.ID, actorUserID,
			actorDeviceID, actorSessionID, true)
		if err == nil {
			if err := tx.Model(&model.ProjectSessionMember{}).Where("id = ?", member.ID).Update("last_seen_at", now).Error; err != nil {
				return err
			}
			member.LastSeenAt = now
		} else if errors.Is(err, ErrNotFound) {
			// Only one active membership per user/device is allowed. A new login
			// on the same installation takes ownership of that durable membership
			// instead of creating a second participant or inheriting it implicitly.
			member, err = s.activeDeviceSessionMemberTx(tx, session.ID,
				actorUserID, actorDeviceID, true)
			if err == nil {
				if err := tx.Model(&model.ProjectSessionMember{}).Where("id = ?", member.ID).
					Updates(map[string]any{"desktop_session_id": actorSessionID,
						"last_seen_at": now}).Error; err != nil {
					return err
				}
				member.DesktopSessionID = &actorSessionID
				member.LastSeenAt = now
			} else if errors.Is(err, ErrNotFound) {
				var activeMembers int64
				if err := tx.Model(&model.ProjectSessionMember{}).
					Where("session_id = ? AND left_at IS NULL", session.ID).
					Count(&activeMembers).Error; err != nil {
					return err
				}
				if activeMembers >= int64(s.MaxParticipants) {
					return ErrSessionFull
				}
				member = model.ProjectSessionMember{
					ID: uuid.New(), SessionID: session.ID, UserID: actorUserID,
					DeviceID: actorDeviceID, DesktopSessionID: &actorSessionID,
					JoinedAt: now, LastSeenAt: now,
				}
				if err := tx.Create(&member).Error; err != nil {
					return err
				}
			} else {
				return err
			}
		} else {
			return err
		}
		if session.HostMemberID == nil {
			candidate, found, err := s.hostCandidateTx(tx, view.Project, session.ID, uuid.Nil)
			if err != nil {
				return err
			}
			if found {
				if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).Updates(map[string]any{
					"host_member_id": candidate.ID, "updated_at": now, "version": gorm.Expr("version + 1"),
				}).Error; err != nil {
					return err
				}
				session.HostMemberID = &candidate.ID
				session.UpdatedAt = now
				session.Version++
			}
		}
		members, err := s.liveMembersTx(tx, session.ID)
		if err != nil {
			return err
		}
		state = SessionState{Session: session, Members: members}
		return nil
	})
	return state, err
}

func (s *Store) HeartbeatSession(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID) (model.ProjectSessionMember, error) {
	var member model.ProjectSessionMember
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, false)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionJoinSession) {
			return ErrForbidden
		}
		if _, err := s.openSessionTx(tx, projectID, sessionID, false); err != nil {
			return err
		}
		member, err = s.activeSessionMemberTx(tx, sessionID, actorUserID,
			actorDeviceID, actorSessionID, true)
		if err != nil {
			return err
		}
		member.LastSeenAt = s.now()
		result := tx.Model(&model.ProjectSessionMember{}).
			Where("id = ? AND left_at IS NULL", member.ID).
			Update("last_seen_at", member.LastSeenAt)
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected != 1 {
			return ErrNotFound
		}
		return nil
	})
	return member, err
}

func (s *Store) LeaveSession(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID) (SessionState, error) {
	var state SessionState
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionJoinSession) {
			return ErrForbidden
		}
		session, err := s.openSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		member, err := s.activeSessionMemberTx(tx, sessionID, actorUserID,
			actorDeviceID, actorSessionID, true)
		if err != nil {
			return err
		}
		now := s.now()
		if err := s.leaveMemberTx(tx, view.Project, &session, member, now); err != nil {
			return err
		}
		if err := tx.First(&session, "id = ?", sessionID).Error; err != nil {
			return err
		}
		members, err := s.liveMembersTx(tx, sessionID)
		if err != nil {
			return err
		}
		state = SessionState{Session: session, Members: members}
		return nil
	})
	return state, err
}

func (s *Store) HandoffHost(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID, targetMemberID uuid.UUID) (SessionState, error) {
	if targetMemberID == uuid.Nil {
		return SessionState{}, invalidf("target member is required")
	}
	var state SessionState
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		session, err := s.openSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		var target model.ProjectSessionMember
		if err := tx.Where("id = ? AND session_id = ? AND left_at IS NULL", targetMemberID, sessionID).First(&target).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		targetRole, err := s.roleForProjectTx(tx, view.Project, target.UserID)
		if err != nil || !RoleAllows(targetRole, PermissionHostSession) {
			return ErrForbidden
		}
		actorIsHost := false
		if session.HostMemberID != nil {
			var host model.ProjectSessionMember
			if tx.First(&host, "id = ?", *session.HostMemberID).Error == nil {
				actorIsHost = sessionMemberMatchesActor(host, actorUserID,
					actorDeviceID, actorSessionID)
			}
		}
		if !actorIsHost && view.Role != model.ProjectRoleOwner {
			return ErrForbidden
		}
		now := s.now()
		if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).Updates(map[string]any{
			"host_member_id": target.ID, "updated_at": now, "version": gorm.Expr("version + 1"),
		}).Error; err != nil {
			return err
		}
		session.HostMemberID = &target.ID
		session.UpdatedAt = now
		session.Version++
		members, err := s.liveMembersTx(tx, sessionID)
		if err != nil {
			return err
		}
		state = SessionState{Session: session, Members: members}
		return nil
	})
	return state, err
}

func (s *Store) EndSession(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID) error {
	_, err := s.BeginEndSession(ctx, projectID, sessionID, actorUserID,
		actorDeviceID, actorSessionID, DefaultSnapshotRetry)
	return err
}

func (s *Store) AcquireTrackLease(ctx context.Context, projectID, sessionID,
	trackID, actorUserID, actorDeviceID, actorSessionID uuid.UUID,
	ttl time.Duration) (model.ProjectTrackLease, error) {
	ttl, err := normalizeLeaseTTL(ttl)
	if err != nil || trackID == uuid.Nil {
		if err != nil {
			return model.ProjectTrackLease{}, err
		}
		return model.ProjectTrackLease{}, invalidf("track is required")
	}
	var lease model.ProjectTrackLease
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionAcquireLease) {
			return ErrForbidden
		}
		if view.Project.Status != model.ProjectActive {
			return ErrProjectInactive
		}
		if _, err := s.liveSessionTx(tx, projectID, sessionID, true); err != nil {
			return err
		}
		member, err := s.activeSessionMemberTx(tx, sessionID, actorUserID,
			actorDeviceID, actorSessionID, true)
		if err != nil {
			return err
		}
		now := s.now()
		lookup := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("project_id = ? AND track_id = ? AND lease_kind = ?", projectID, trackID, model.TrackLeaseRecord).
			First(&lease)
		if lookup.Error == nil && lease.HolderMemberID != member.ID && lease.ExpiresAt.After(now) {
			return &LeaseHeldError{HolderMemberID: lease.HolderMemberID, ExpiresAt: lease.ExpiresAt}
		}
		if lookup.Error != nil && !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
			return lookup.Error
		}
		if errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
			lease = model.ProjectTrackLease{
				ID: uuid.New(), ProjectID: projectID, SessionID: sessionID, TrackID: trackID,
				LeaseKind: model.TrackLeaseRecord, HolderMemberID: member.ID,
				AcquiredAt: now, RenewedAt: now, ExpiresAt: now.Add(ttl),
			}
			return tx.Create(&lease).Error
		}
		newID := lease.ID
		acquiredAt := lease.AcquiredAt
		if lease.HolderMemberID != member.ID || !lease.ExpiresAt.After(now) {
			newID = uuid.New()
			acquiredAt = now
		}
		if err := tx.Model(&model.ProjectTrackLease{}).Where("id = ?", lease.ID).Updates(map[string]any{
			"id": newID, "session_id": sessionID, "holder_member_id": member.ID,
			"acquired_at": acquiredAt, "renewed_at": now, "expires_at": now.Add(ttl),
		}).Error; err != nil {
			return err
		}
		lease.ID = newID
		lease.SessionID = sessionID
		lease.HolderMemberID = member.ID
		lease.AcquiredAt = acquiredAt
		lease.RenewedAt = now
		lease.ExpiresAt = now.Add(ttl)
		return nil
	})
	return lease, err
}

func (s *Store) RenewTrackLease(ctx context.Context, projectID, sessionID,
	leaseID, actorUserID, actorDeviceID, actorSessionID uuid.UUID,
	ttl time.Duration) (model.ProjectTrackLease, error) {
	return s.renewTrackLease(ctx, projectID, sessionID, leaseID, uuid.Nil,
		actorUserID, actorDeviceID, actorSessionID, ttl)
}

func (s *Store) RenewTrackLeaseForTrack(ctx context.Context, projectID, sessionID,
	leaseID, expectedTrackID, actorUserID, actorDeviceID, actorSessionID uuid.UUID,
	ttl time.Duration) (model.ProjectTrackLease, error) {
	if expectedTrackID == uuid.Nil {
		return model.ProjectTrackLease{}, invalidf("lease track is required")
	}
	return s.renewTrackLease(ctx, projectID, sessionID, leaseID, expectedTrackID,
		actorUserID, actorDeviceID, actorSessionID, ttl)
}

func (s *Store) renewTrackLease(ctx context.Context, projectID, sessionID,
	leaseID, expectedTrackID, actorUserID, actorDeviceID, actorSessionID uuid.UUID,
	ttl time.Duration) (model.ProjectTrackLease, error) {
	ttl, err := normalizeLeaseTTL(ttl)
	if err != nil {
		return model.ProjectTrackLease{}, err
	}
	var lease model.ProjectTrackLease
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionAcquireLease) {
			return ErrForbidden
		}
		if view.Project.Status != model.ProjectActive {
			return ErrProjectInactive
		}
		if _, err := s.liveSessionTx(tx, projectID, sessionID, true); err != nil {
			return err
		}
		member, err := s.activeSessionMemberTx(tx, sessionID, actorUserID,
			actorDeviceID, actorSessionID, true)
		if err != nil {
			return err
		}
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND project_id = ? AND session_id = ?", leaseID, projectID, sessionID).First(&lease).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		if err := validateExpectedLeaseTrack(lease.TrackID, expectedTrackID); err != nil {
			return err
		}
		if lease.HolderMemberID != member.ID {
			return ErrForbidden
		}
		now := s.now()
		if !lease.ExpiresAt.After(now) {
			return ErrLeaseExpired
		}
		lease.RenewedAt, lease.ExpiresAt = now, now.Add(ttl)
		return tx.Model(&model.ProjectTrackLease{}).Where("id = ?", lease.ID).Updates(map[string]any{
			"renewed_at": lease.RenewedAt, "expires_at": lease.ExpiresAt,
		}).Error
	})
	return lease, err
}

func (s *Store) ReleaseTrackLease(ctx context.Context, projectID, sessionID,
	leaseID, actorUserID, actorDeviceID, actorSessionID uuid.UUID) error {
	return s.releaseTrackLease(ctx, projectID, sessionID, leaseID, uuid.Nil,
		actorUserID, actorDeviceID, actorSessionID)
}

func (s *Store) ReleaseTrackLeaseForTrack(ctx context.Context, projectID, sessionID,
	leaseID, expectedTrackID, actorUserID, actorDeviceID, actorSessionID uuid.UUID) error {
	if expectedTrackID == uuid.Nil {
		return invalidf("lease track is required")
	}
	return s.releaseTrackLease(ctx, projectID, sessionID, leaseID, expectedTrackID,
		actorUserID, actorDeviceID, actorSessionID)
}

func (s *Store) releaseTrackLease(ctx context.Context, projectID, sessionID,
	leaseID, expectedTrackID, actorUserID, actorDeviceID, actorSessionID uuid.UUID) error {
	return s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		session, err := s.liveSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		member, memberErr := s.activeSessionMemberTx(tx, sessionID, actorUserID,
			actorDeviceID, actorSessionID, true)
		if memberErr != nil && !errors.Is(memberErr, ErrNotFound) {
			return memberErr
		}
		var lease model.ProjectTrackLease
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND project_id = ? AND session_id = ?", leaseID, projectID, sessionID).First(&lease).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		if err := validateExpectedLeaseTrack(lease.TrackID, expectedTrackID); err != nil {
			return err
		}
		allowed := view.Role == model.ProjectRoleOwner
		if memberErr == nil {
			allowed = allowed || lease.HolderMemberID == member.ID ||
				(session.HostMemberID != nil && *session.HostMemberID == member.ID)
		}
		if !allowed {
			return ErrForbidden
		}
		return tx.Delete(&model.ProjectTrackLease{}, "id = ?", lease.ID).Error
	})
}

func validateExpectedLeaseTrack(actualTrackID, expectedTrackID uuid.UUID) error {
	if expectedTrackID != uuid.Nil && actualTrackID != expectedTrackID {
		return invalidf("lease track does not match the requested track")
	}
	return nil
}

func (s *Store) sessionState(ctx context.Context, session model.ProjectSession) (SessionState, error) {
	members, err := s.liveMembersTx(s.DB.WithContext(ctx), session.ID)
	if err != nil {
		return SessionState{}, err
	}
	return SessionState{Session: session, Members: members}, nil
}

func (s *Store) liveSessionTx(tx *gorm.DB, projectID, sessionID uuid.UUID, lock bool) (model.ProjectSession, error) {
	session, err := s.openSessionTx(tx, projectID, sessionID, lock)
	if err != nil {
		return model.ProjectSession{}, err
	}
	if session.Status != model.ProjectSessionActive {
		return model.ProjectSession{}, ErrSessionEnded
	}
	return session, nil
}

func (s *Store) openSessionTx(tx *gorm.DB, projectID, sessionID uuid.UUID,
	lock bool) (model.ProjectSession, error) {
	query := tx
	if lock {
		query = query.Clauses(clause.Locking{Strength: "UPDATE"})
	}
	var session model.ProjectSession
	if err := query.Where("id = ? AND project_id = ?", sessionID, projectID).First(&session).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return model.ProjectSession{}, ErrNotFound
		}
		return model.ProjectSession{}, err
	}
	if session.Status != model.ProjectSessionActive && session.Status != model.ProjectSessionEnding {
		return model.ProjectSession{}, ErrSessionEnded
	}
	return session, nil
}

func (s *Store) activeSessionMemberTx(tx *gorm.DB, sessionID, userID, deviceID,
	desktopSessionID uuid.UUID, lock bool) (model.ProjectSessionMember, error) {
	if desktopSessionID == uuid.Nil {
		return model.ProjectSessionMember{}, ErrNotFound
	}
	query := tx
	if lock {
		query = query.Clauses(clause.Locking{Strength: "UPDATE"})
	}
	var member model.ProjectSessionMember
	if err := query.Where(`session_id = ? AND user_id = ? AND device_id = ?
		AND desktop_session_id = ? AND left_at IS NULL`, sessionID, userID,
		deviceID, desktopSessionID).First(&member).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return model.ProjectSessionMember{}, ErrNotFound
		}
		return model.ProjectSessionMember{}, err
	}
	return member, nil
}

func (s *Store) activeDeviceSessionMemberTx(tx *gorm.DB, sessionID, userID,
	deviceID uuid.UUID, lock bool) (model.ProjectSessionMember, error) {
	query := tx
	if lock {
		query = query.Clauses(clause.Locking{Strength: "UPDATE"})
	}
	var member model.ProjectSessionMember
	if err := query.Where(`session_id = ? AND user_id = ? AND device_id = ?
		AND left_at IS NULL`, sessionID, userID, deviceID).First(&member).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return model.ProjectSessionMember{}, ErrNotFound
		}
		return model.ProjectSessionMember{}, err
	}
	return member, nil
}

func sessionMemberMatchesActor(member model.ProjectSessionMember, userID,
	deviceID, desktopSessionID uuid.UUID) bool {
	return member.UserID == userID && member.DeviceID == deviceID &&
		member.DesktopSessionID != nil &&
		*member.DesktopSessionID == desktopSessionID
}

func (s *Store) requireActiveActorTx(tx *gorm.DB, userID, deviceID,
	desktopSessionID uuid.UUID) error {
	if tx == nil || userID == uuid.Nil || deviceID == uuid.Nil ||
		desktopSessionID == uuid.Nil {
		return ErrForbidden
	}
	// Share locks close the middleware/transaction race: a concurrent suspend
	// or device revoke either commits first (and this lookup fails) or waits,
	// then evicts the membership created by this transaction before it commits.
	var user model.User
	if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
		Where("id = ? AND status = ?", userID, model.UserActive).
		First(&user).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return ErrForbidden
		}
		return err
	}
	var device model.Device
	if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
		Where("id = ? AND user_id = ? AND revoked_at IS NULL", deviceID, userID).
		First(&device).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return ErrForbidden
		}
		return err
	}
	var session model.DesktopSession
	if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
		Where("id = ? AND user_id = ? AND device_id = ? AND revoked_at IS NULL AND expires_at > ?",
			desktopSessionID, userID, deviceID, s.now()).
		First(&session).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return ErrForbidden
		}
		return err
	}
	return nil
}

func (s *Store) liveMembersTx(tx *gorm.DB, sessionID uuid.UUID) ([]model.ProjectSessionMember, error) {
	var members []model.ProjectSessionMember
	err := tx.Where("session_id = ? AND left_at IS NULL", sessionID).Order("joined_at, id").Find(&members).Error
	return members, err
}

func (s *Store) leaveMemberTx(tx *gorm.DB, project model.CloudProject, session *model.ProjectSession, member model.ProjectSessionMember, now time.Time) error {
	if err := tx.Model(&model.ProjectSessionMember{}).Where("id = ? AND left_at IS NULL", member.ID).
		Update("left_at", now).Error; err != nil {
		return err
	}
	if err := tx.Where("holder_member_id = ?", member.ID).Delete(&model.ProjectTrackLease{}).Error; err != nil {
		return err
	}
	if session.HostMemberID == nil || *session.HostMemberID != member.ID {
		if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).
			Updates(map[string]any{
				"updated_at": now, "version": gorm.Expr("version + 1"),
			}).Error; err != nil {
			return err
		}
		session.UpdatedAt = now
		session.Version++
		return nil
	}
	candidate, found, err := s.hostCandidateTx(tx, project, session.ID, member.ID)
	if err != nil {
		return err
	}
	if !found {
		if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).Updates(map[string]any{
			"host_member_id": nil, "updated_at": now, "version": gorm.Expr("version + 1"),
		}).Error; err != nil {
			return err
		}
		session.HostMemberID = nil
		session.UpdatedAt = now
		session.Version++
		return nil
	}
	if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).Updates(map[string]any{
		"host_member_id": candidate.ID, "updated_at": now, "version": gorm.Expr("version + 1"),
	}).Error; err != nil {
		return err
	}
	session.HostMemberID = &candidate.ID
	session.UpdatedAt = now
	session.Version++
	return nil
}

// EndIdleSessions advances active rooms which have remained empty for the
// reconnect grace period. An exact-head snapshot closes immediately; otherwise
// the room enters ending with a durable session_end snapshot request. With no
// server-side musical reducer it is safer to wait for an editor recovery join
// than to falsely declare an unsnapshotted session complete.
func (s *Store) EndIdleSessions(ctx context.Context, reconnectGrace time.Duration) (
	[]EndedIdleSession, error) {
	if reconnectGrace < 5*time.Second || reconnectGrace > 5*time.Minute {
		return nil, invalidf("reconnect grace must be between 5 seconds and 5 minutes")
	}
	cutoff := s.now().Add(-reconnectGrace)
	var ended []EndedIdleSession
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		var candidates []model.ProjectSession
		if err := tx.Where(`status = ? AND updated_at <= ? AND NOT EXISTS (
				SELECT 1 FROM project_session_members AS members
				WHERE members.session_id = project_live_sessions.id
				AND members.left_at IS NULL
			)`, model.ProjectSessionActive, cutoff).
			Order("project_id, id").Find(&candidates).Error; err != nil {
			return err
		}
		for index := range candidates {
			var project model.CloudProject
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				First(&project, "id = ?", candidates[index].ProjectID).Error; err != nil {
				return err
			}
			var session model.ProjectSession
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE", Options: "SKIP LOCKED"}).
				First(&session, "id = ?", candidates[index].ID).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					continue
				}
				return err
			}
			var liveMembers int64
			if err := tx.Model(&model.ProjectSessionMember{}).
				Where("session_id = ? AND left_at IS NULL", session.ID).
				Count(&liveMembers).Error; err != nil {
				return err
			}
			if liveMembers != 0 || session.Status != model.ProjectSessionActive ||
				session.UpdatedAt.After(cutoff) {
				continue
			}
			finalized := project.SnapshotSeq == project.HeadSeq
			if finalized {
				if err := s.endSessionTx(tx, &session, s.now()); err != nil {
					return err
				}
			} else {
				now := s.now()
				if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).
					Updates(map[string]any{
						"status": model.ProjectSessionEnding, "updated_at": now,
						"version": gorm.Expr("version + 1"),
					}).Error; err != nil {
					return err
				}
				session.Status = model.ProjectSessionEnding
				session.UpdatedAt = now
				session.Version++
				if _, err := ensureSessionEndSnapshotRequestTx(tx, project, session,
					now); err != nil {
					return err
				}
			}
			ended = append(ended, EndedIdleSession{
				ProjectID: project.ID, SessionID: session.ID,
				FinalSeq: project.HeadSeq, Finalized: finalized,
			})
		}
		return nil
	})
	return ended, err
}

// ReapStaleSessionMembers removes active participants whose authenticated
// heartbeat has stopped. Each room is rechecked under the project/session/member
// locks used by normal lifecycle mutations; lease deletion and host handoff are
// therefore committed atomically with the leave timestamp. Returned events are
// safe to publish only after this method returns successfully.
func (s *Store) ReapStaleSessionMembers(ctx context.Context,
	staleAfter time.Duration, limit int) ([]ReapedSessionMembers, error) {
	if err := validateReaperParameters(staleAfter, limit); err != nil {
		return nil, err
	}
	cutoff := s.now().Add(-staleAfter)
	type candidate struct {
		ID        uuid.UUID
		SessionID uuid.UUID
		ProjectID uuid.UUID
	}
	type candidateGroup struct {
		ProjectID uuid.UUID
		SessionID uuid.UUID
		MemberIDs []uuid.UUID
	}
	var events []ReapedSessionMembers
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		var candidates []candidate
		if err := tx.Table("project_session_members AS members").
			Select("members.id, members.session_id, sessions.project_id").
			Joins("JOIN project_live_sessions AS sessions ON sessions.id = members.session_id").
			Where("members.left_at IS NULL AND members.last_seen_at <= ? AND sessions.status IN ?",
				cutoff, []string{model.ProjectSessionActive, model.ProjectSessionEnding}).
			Order("sessions.project_id, members.session_id, members.last_seen_at, members.id").
			Limit(limit).Scan(&candidates).Error; err != nil {
			return err
		}
		groups := make([]candidateGroup, 0, len(candidates))
		for _, candidate := range candidates {
			if len(groups) == 0 || groups[len(groups)-1].ProjectID != candidate.ProjectID ||
				groups[len(groups)-1].SessionID != candidate.SessionID {
				groups = append(groups, candidateGroup{
					ProjectID: candidate.ProjectID, SessionID: candidate.SessionID,
				})
			}
			groups[len(groups)-1].MemberIDs = append(groups[len(groups)-1].MemberIDs,
				candidate.ID)
		}
		for _, group := range groups {
			var project model.CloudProject
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				First(&project, "id = ?", group.ProjectID).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					continue
				}
				return err
			}
			var session model.ProjectSession
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				Where("id = ? AND project_id = ? AND status IN ?", group.SessionID,
					group.ProjectID, []string{model.ProjectSessionActive,
						model.ProjectSessionEnding}).
				First(&session).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					continue
				}
				return err
			}
			previousHost := copyOptionalUUID(session.HostMemberID)
			var staleMembers []model.ProjectSessionMember
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				Where("id IN ? AND session_id = ? AND left_at IS NULL AND last_seen_at <= ?",
					group.MemberIDs, group.SessionID, cutoff).
				Order("joined_at, id").Find(&staleMembers).Error; err != nil {
				return err
			}
			if len(staleMembers) == 0 {
				continue
			}
			now := s.now()
			memberIDs := make([]uuid.UUID, 0, len(staleMembers))
			for _, member := range staleMembers {
				if err := s.leaveMemberTx(tx, project, &session, member, now); err != nil {
					return err
				}
				memberIDs = append(memberIDs, member.ID)
			}
			events = append(events, ReapedSessionMembers{
				ProjectID: group.ProjectID, SessionID: group.SessionID,
				MemberIDs: memberIDs, PreviousHostMemberID: previousHost,
				HostMemberID: copyOptionalUUID(session.HostMemberID),
			})
		}
		return nil
	})
	return events, err
}

func validateReaperParameters(staleAfter time.Duration, limit int) error {
	if staleAfter < MinimumMemberStaleAfter || staleAfter > MaximumMemberStaleAfter {
		return invalidf("member stale duration must be between thirty seconds and ten minutes")
	}
	if limit < 1 || limit > MaximumReaperBatch {
		return invalidf("reaper batch must contain between one and one thousand members")
	}
	return nil
}

func copyOptionalUUID(value *uuid.UUID) *uuid.UUID {
	if value == nil {
		return nil
	}
	copy := *value
	return &copy
}

func (s *Store) hostCandidateTx(tx *gorm.DB, project model.CloudProject, sessionID, excludedMemberID uuid.UUID) (model.ProjectSessionMember, bool, error) {
	var members []model.ProjectSessionMember
	if err := tx.Where("session_id = ? AND left_at IS NULL AND id <> ?", sessionID, excludedMemberID).
		Order("joined_at, id").Find(&members).Error; err != nil {
		return model.ProjectSessionMember{}, false, err
	}
	for _, member := range members {
		role, err := s.roleForProjectTx(tx, project, member.UserID)
		if errors.Is(err, ErrForbidden) {
			continue
		}
		if err != nil {
			return model.ProjectSessionMember{}, false, err
		}
		if RoleAllows(role, PermissionHostSession) {
			return member, true, nil
		}
	}
	return model.ProjectSessionMember{}, false, nil
}

func (s *Store) endSessionTx(tx *gorm.DB, session *model.ProjectSession, now time.Time) error {
	if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).Updates(map[string]any{
		"status": model.ProjectSessionEnded, "host_member_id": nil, "ended_at": now,
		"updated_at": now, "version": gorm.Expr("version + 1"),
	}).Error; err != nil {
		return err
	}
	if err := tx.Model(&model.ProjectSessionMember{}).Where("session_id = ? AND left_at IS NULL", session.ID).
		Update("left_at", now).Error; err != nil {
		return err
	}
	if err := tx.Where("session_id = ?", session.ID).Delete(&model.ProjectTrackLease{}).Error; err != nil {
		return err
	}
	session.Status = model.ProjectSessionEnded
	session.HostMemberID = nil
	session.EndedAt = &now
	session.UpdatedAt = now
	session.Version++
	return nil
}

func (s *Store) removeUserFromLiveSessionsTx(tx *gorm.DB, project model.CloudProject, userID uuid.UUID, now time.Time) error {
	var sessions []model.ProjectSession
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("project_id = ? AND status = ?", project.ID, model.ProjectSessionActive).Find(&sessions).Error; err != nil {
		return err
	}
	for index := range sessions {
		var members []model.ProjectSessionMember
		if err := tx.Where("session_id = ? AND user_id = ? AND left_at IS NULL", sessions[index].ID, userID).
			Find(&members).Error; err != nil {
			return err
		}
		for _, member := range members {
			if err := s.leaveMemberTx(tx, project, &sessions[index], member, now); err != nil {
				return err
			}
		}
	}
	return nil
}

func (s *Store) reconcileProjectHostsTx(tx *gorm.DB, project model.CloudProject, userID uuid.UUID, now time.Time) error {
	var sessions []model.ProjectSession
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("project_id = ? AND status = ?", project.ID, model.ProjectSessionActive).Find(&sessions).Error; err != nil {
		return err
	}
	for index := range sessions {
		if sessions[index].HostMemberID == nil {
			continue
		}
		var host model.ProjectSessionMember
		if err := tx.First(&host, "id = ?", *sessions[index].HostMemberID).Error; err != nil || host.UserID != userID {
			continue
		}
		candidate, found, err := s.hostCandidateTx(tx, project, sessions[index].ID, host.ID)
		if err != nil {
			return err
		}
		if !found {
			if err := tx.Model(&model.ProjectSession{}).Where("id = ?", sessions[index].ID).Updates(map[string]any{
				"host_member_id": nil, "updated_at": now, "version": gorm.Expr("version + 1"),
			}).Error; err != nil {
				return err
			}
			continue
		}
		if err := tx.Model(&model.ProjectSession{}).Where("id = ?", sessions[index].ID).Updates(map[string]any{
			"host_member_id": candidate.ID, "updated_at": now, "version": gorm.Expr("version + 1"),
		}).Error; err != nil {
			return err
		}
	}
	return nil
}

// EvictDeviceSessionsTx removes every live room membership owned by a revoked
// device inside the caller's transaction. Besides marking the participant as
// left, leaveMemberTx releases recording leases and transfers the host to the
// oldest remaining editor (or leaves the room read-only when there is none).
func (s *Store) EvictDeviceSessionsTx(tx *gorm.DB, deviceID uuid.UUID, now time.Time) error {
	if tx == nil || deviceID == uuid.Nil {
		return invalidf("device is required for collaboration eviction")
	}
	return s.evictSessionMembersTx(tx, uuid.Nil, deviceID, uuid.Nil, now.UTC())
}

// EvictUserSessionsTx is the account-wide counterpart used by suspend,
// session-revoke and delete workflows. It intentionally runs in the same
// transaction as the account mutation so neither half can succeed alone.
func (s *Store) EvictUserSessionsTx(tx *gorm.DB, userID uuid.UUID, now time.Time) error {
	if tx == nil || userID == uuid.Nil {
		return invalidf("user is required for collaboration eviction")
	}
	return s.evictSessionMembersTx(tx, userID, uuid.Nil, uuid.Nil, now.UTC())
}

// EvictDesktopSessionTx is the narrow counterpart used by refresh rotation
// and logout. It must be committed in the same transaction that revokes the
// credential so another desktop session on the same installation remains live.
func (s *Store) EvictDesktopSessionTx(tx *gorm.DB, desktopSessionID uuid.UUID,
	now time.Time) error {
	if tx == nil || desktopSessionID == uuid.Nil {
		return invalidf("desktop session is required for collaboration eviction")
	}
	return s.evictSessionMembersTx(tx, uuid.Nil, uuid.Nil, desktopSessionID,
		now.UTC())
}

func (s *Store) evictSessionMembersTx(tx *gorm.DB, userID, deviceID,
	desktopSessionID uuid.UUID, now time.Time) error {
	var projectIDs []uuid.UUID
	projects := tx.Table("project_session_members AS members").
		Select("DISTINCT sessions.project_id").
		Joins("JOIN project_live_sessions AS sessions ON sessions.id = members.session_id").
		Where("sessions.status = ? AND members.left_at IS NULL",
			model.ProjectSessionActive)
	if userID != uuid.Nil {
		projects = projects.Where("members.user_id = ?", userID)
	}
	if deviceID != uuid.Nil {
		projects = projects.Where("members.device_id = ?", deviceID)
	}
	if desktopSessionID != uuid.Nil {
		projects = projects.Where("members.desktop_session_id = ?", desktopSessionID)
	}
	if err := projects.Order("sessions.project_id").
		Pluck("sessions.project_id", &projectIDs).Error; err != nil {
		return err
	}

	for _, projectID := range projectIDs {
		var project model.CloudProject
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			First(&project, "id = ?", projectID).Error; err != nil {
			return err
		}
		var sessions []model.ProjectSession
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("project_id = ? AND status = ?", projectID,
				model.ProjectSessionActive).
			Order("id").Find(&sessions).Error; err != nil {
			return err
		}
		for sessionIndex := range sessions {
			members := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				Where("session_id = ? AND left_at IS NULL", sessions[sessionIndex].ID)
			if userID != uuid.Nil {
				members = members.Where("user_id = ?", userID)
			}
			if deviceID != uuid.Nil {
				members = members.Where("device_id = ?", deviceID)
			}
			if desktopSessionID != uuid.Nil {
				members = members.Where("desktop_session_id = ?", desktopSessionID)
			}
			var targets []model.ProjectSessionMember
			if err := members.Order("id").Find(&targets).Error; err != nil {
				return err
			}
			for _, member := range targets {
				if err := s.leaveMemberTx(tx, project, &sessions[sessionIndex],
					member, now); err != nil {
					return err
				}
			}
		}
	}
	return nil
}
