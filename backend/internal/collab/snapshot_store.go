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

const (
	DefaultSnapshotRetry = 15 * time.Second
	MaximumSnapshotBatch = 1000
)

// SnapshotDispatch is safe room-control metadata. It intentionally contains no
// project document bytes, filenames, hashes or provider upload identifiers.
type SnapshotDispatch struct {
	RequestID    uuid.UUID `json:"requestId"`
	ProjectID    uuid.UUID `json:"-"`
	SessionID    uuid.UUID `json:"sessionId"`
	HostMemberID uuid.UUID `json:"hostParticipantId"`
	TargetSeq    int64     `json:"targetServerSeq"`
	Reason       string    `json:"reason"`
	Attempt      int       `json:"attempt"`
	RetryAtMs    int64     `json:"retryAtMs"`
}

type SessionEndResult struct {
	ProjectID uuid.UUID
	SessionID uuid.UUID
	FinalSeq  int64
	Finalized bool
	Dispatch  *SnapshotDispatch
}

type SnapshotFinalization struct {
	ProjectID uuid.UUID `json:"projectId"`
	SessionID uuid.UUID `json:"sessionId"`
	FinalSeq  int64     `json:"finalServerSeq"`
}

func normalizeSnapshotMaintenance(opsThreshold int64, ageThreshold,
	retryDelay time.Duration, limit int) (int64, time.Duration, time.Duration, int, error) {
	if opsThreshold < 1 || opsThreshold > 100000 {
		return 0, 0, 0, 0, invalidf("snapshot operation threshold is outside configured bounds")
	}
	if ageThreshold < 30*time.Second || ageThreshold > 24*time.Hour {
		return 0, 0, 0, 0, invalidf("snapshot age threshold is outside configured bounds")
	}
	if retryDelay < 5*time.Second || retryDelay > 5*time.Minute {
		return 0, 0, 0, 0, invalidf("snapshot retry delay is outside configured bounds")
	}
	if limit < 1 || limit > MaximumSnapshotBatch {
		return 0, 0, 0, 0, invalidf("snapshot batch is outside configured bounds")
	}
	return opsThreshold, ageThreshold, retryDelay, limit, nil
}

// ScheduleSnapshotRequests creates threshold-triggered jobs and redelivers due
// jobs. The exact target is frozen for session_end. Autosave jobs are
// superseded when more operations arrive, preventing an old in-flight upload
// from advancing project.snapshot_seq under a misleading request identity.
func (s *Store) ScheduleSnapshotRequests(ctx context.Context, opsThreshold int64,
	ageThreshold, retryDelay time.Duration, limit int) ([]SnapshotDispatch, error) {
	opsThreshold, ageThreshold, retryDelay, limit, err := normalizeSnapshotMaintenance(
		opsThreshold, ageThreshold, retryDelay, limit)
	if err != nil {
		return nil, err
	}
	now := s.now()
	var dispatches []SnapshotDispatch
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		var due []model.ProjectSnapshotRequest
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE", Options: "SKIP LOCKED"}).
			Where("status = ? AND next_retry_at <= ?", model.SnapshotRequestPending, now).
			Order("next_retry_at, project_id, id").Limit(limit).Find(&due).Error; err != nil {
			return err
		}
		for index := range due {
			request := due[index]
			var project model.CloudProject
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				First(&project, "id = ?", request.ProjectID).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					continue
				}
				return err
			}
			var session model.ProjectSession
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				Where("id = ? AND project_id = ? AND status IN ?", request.SessionID,
					request.ProjectID, []string{model.ProjectSessionActive, model.ProjectSessionEnding}).
				First(&session).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					if err := tx.Model(&request).Updates(map[string]any{
						"status": model.SnapshotRequestSuperseded,
					}).Error; err != nil {
						return err
					}
					continue
				}
				return err
			}
			if request.Reason == model.SnapshotReasonAutosave && project.HeadSeq != request.TargetSeq {
				if err := tx.Model(&request).Update("status", model.SnapshotRequestSuperseded).Error; err != nil {
					return err
				}
				if project.HeadSeq <= project.SnapshotSeq {
					continue
				}
				request = model.ProjectSnapshotRequest{
					ID: uuid.New(), ProjectID: project.ID, SessionID: session.ID,
					TargetSeq: project.HeadSeq, Reason: model.SnapshotReasonAutosave,
					Status: model.SnapshotRequestPending, RequestedAt: now,
					NextRetryAt: now,
				}
				if err := tx.Create(&request).Error; err != nil {
					return err
				}
			}
			dispatch, sent, err := s.dispatchSnapshotRequestTx(tx, &request, session,
				now, retryDelay)
			if err != nil {
				return err
			}
			if sent {
				dispatches = append(dispatches, dispatch)
			}
			if len(dispatches) >= limit {
				return nil
			}
		}

		remaining := limit - len(dispatches)
		if remaining <= 0 {
			return nil
		}
		type candidate struct {
			ProjectID uuid.UUID
			SessionID uuid.UUID
		}
		var candidates []candidate
		cutoff := now.Add(-ageThreshold)
		if err := tx.Raw(`
			SELECT projects.id AS project_id, sessions.id AS session_id
			FROM cloud_projects AS projects
			JOIN project_live_sessions AS sessions
			  ON sessions.project_id = projects.id AND sessions.status = ?
			WHERE projects.status IN (?, ?)
			  AND projects.head_seq > projects.snapshot_seq
			  AND sessions.host_member_id IS NOT NULL
			  AND NOT EXISTS (
			      SELECT 1 FROM project_snapshot_requests AS requests
			      WHERE requests.project_id = projects.id AND requests.status = ?
			  )
			  AND (
			      projects.head_seq - projects.snapshot_seq >= ?
			      OR COALESCE((
			          SELECT MAX(snapshots.created_at)
			          FROM project_snapshots AS snapshots
			          WHERE snapshots.project_id = projects.id
			            AND snapshots.seq <= projects.snapshot_seq
			      ), sessions.started_at, sessions.created_at) <= ?
			  )
			ORDER BY projects.updated_at, projects.id
			LIMIT ?`, model.ProjectSessionActive, model.ProjectActive, model.ProjectConflict,
			model.SnapshotRequestPending, opsThreshold, cutoff, remaining).
			Scan(&candidates).Error; err != nil {
			return err
		}
		for _, candidate := range candidates {
			var project model.CloudProject
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				First(&project, "id = ?", candidate.ProjectID).Error; err != nil {
				return err
			}
			var pending int64
			if err := tx.Model(&model.ProjectSnapshotRequest{}).
				Where("project_id = ? AND status = ?", project.ID, model.SnapshotRequestPending).
				Count(&pending).Error; err != nil {
				return err
			}
			if pending != 0 || project.HeadSeq <= project.SnapshotSeq {
				continue
			}
			var session model.ProjectSession
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				Where("id = ? AND status = ?", candidate.SessionID, model.ProjectSessionActive).
				First(&session).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					continue
				}
				return err
			}
			request := model.ProjectSnapshotRequest{
				ID: uuid.New(), ProjectID: project.ID, SessionID: session.ID,
				TargetSeq: project.HeadSeq, Reason: model.SnapshotReasonAutosave,
				Status: model.SnapshotRequestPending, RequestedAt: now, NextRetryAt: now,
			}
			if err := tx.Create(&request).Error; err != nil {
				return err
			}
			dispatch, sent, err := s.dispatchSnapshotRequestTx(tx, &request, session,
				now, retryDelay)
			if err != nil {
				return err
			}
			if sent {
				dispatches = append(dispatches, dispatch)
			}
		}
		return nil
	})
	return dispatches, err
}

func (s *Store) dispatchSnapshotRequestTx(tx *gorm.DB,
	request *model.ProjectSnapshotRequest, session model.ProjectSession, now time.Time,
	retryDelay time.Duration) (SnapshotDispatch, bool, error) {
	if session.HostMemberID == nil {
		if err := tx.Model(request).Update("next_retry_at", now.Add(retryDelay)).Error; err != nil {
			return SnapshotDispatch{}, false, err
		}
		return SnapshotDispatch{}, false, nil
	}
	var host model.ProjectSessionMember
	if err := tx.Where("id = ? AND session_id = ? AND left_at IS NULL",
		*session.HostMemberID, session.ID).First(&host).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			if err := tx.Model(request).Updates(map[string]any{
				"assigned_member_id": nil, "next_retry_at": now.Add(retryDelay),
			}).Error; err != nil {
				return SnapshotDispatch{}, false, err
			}
			return SnapshotDispatch{}, false, nil
		}
		return SnapshotDispatch{}, false, err
	}
	attempt := request.AttemptCount + 1
	next := now.Add(retryDelay)
	if err := tx.Model(request).Updates(map[string]any{
		"assigned_member_id": host.ID, "attempt_count": attempt,
		"last_dispatched_at": now, "next_retry_at": next,
	}).Error; err != nil {
		return SnapshotDispatch{}, false, err
	}
	request.AssignedMemberID = &host.ID
	request.AttemptCount = attempt
	request.LastDispatchedAt = &now
	request.NextRetryAt = next
	return SnapshotDispatch{
		RequestID: request.ID, ProjectID: request.ProjectID, SessionID: request.SessionID,
		HostMemberID: host.ID, TargetSeq: request.TargetSeq, Reason: request.Reason,
		Attempt: attempt, RetryAtMs: next.UnixMilli(),
	}, true, nil
}

func ensureSessionEndSnapshotRequestTx(tx *gorm.DB, project model.CloudProject,
	session model.ProjectSession, now time.Time) (model.ProjectSnapshotRequest, error) {
	var request model.ProjectSnapshotRequest
	lookup := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("project_id = ? AND status = ?", project.ID,
			model.SnapshotRequestPending).First(&request)
	if lookup.Error == nil {
		if request.Reason == model.SnapshotReasonSessionEnd &&
			request.TargetSeq == project.HeadSeq && request.SessionID == session.ID {
			return request, nil
		}
		if err := tx.Model(&request).
			Update("status", model.SnapshotRequestSuperseded).Error; err != nil {
			return model.ProjectSnapshotRequest{}, err
		}
	} else if !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
		return model.ProjectSnapshotRequest{}, lookup.Error
	}
	request = model.ProjectSnapshotRequest{
		ID: uuid.New(), ProjectID: project.ID, SessionID: session.ID,
		TargetSeq: project.HeadSeq, Reason: model.SnapshotReasonSessionEnd,
		Status: model.SnapshotRequestPending, RequestedAt: now, NextRetryAt: now,
	}
	if err := tx.Create(&request).Error; err != nil {
		return model.ProjectSnapshotRequest{}, err
	}
	return request, nil
}

func (s *Store) BeginEndSession(ctx context.Context, projectID, sessionID,
	actorUserID, actorDeviceID, actorSessionID uuid.UUID,
	retryDelay time.Duration) (SessionEndResult, error) {
	if retryDelay < 5*time.Second || retryDelay > 5*time.Minute {
		return SessionEndResult{}, invalidf("snapshot retry delay is outside configured bounds")
	}
	result := SessionEndResult{ProjectID: projectID, SessionID: sessionID}
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, actorUserID, actorDeviceID,
			actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		var session model.ProjectSession
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND project_id = ? AND status IN ?", sessionID, projectID,
				[]string{model.ProjectSessionActive, model.ProjectSessionEnding}).
			First(&session).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrSessionEnded
			}
			return err
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
		result.FinalSeq = view.Project.HeadSeq
		if view.Project.SnapshotSeq == view.Project.HeadSeq {
			if err := s.endSessionTx(tx, &session, s.now()); err != nil {
				return err
			}
			result.Finalized = true
			return nil
		}
		if session.HostMemberID == nil {
			return ErrConflict
		}
		now := s.now()
		if session.Status != model.ProjectSessionEnding {
			if err := tx.Model(&session).Updates(map[string]any{
				"status": model.ProjectSessionEnding, "updated_at": now,
				"version": gorm.Expr("version + 1"),
			}).Error; err != nil {
				return err
			}
			session.Status = model.ProjectSessionEnding
		}
		request, err := ensureSessionEndSnapshotRequestTx(tx, view.Project, session, now)
		if err != nil {
			return err
		}
		if request.LastDispatchedAt != nil && request.NextRetryAt.After(now) {
			return nil
		}
		dispatch, sent, err := s.dispatchSnapshotRequestTx(tx, &request, session,
			now, retryDelay)
		if err != nil {
			return err
		}
		if sent {
			result.Dispatch = &dispatch
		}
		return nil
	})
	return result, err
}

func pendingSnapshotRequestTx(tx *gorm.DB, projectID uuid.UUID, targetSeq int64,
	actorUserID, deviceID, actorSessionID uuid.UUID, lock bool) (model.ProjectSnapshotRequest, error) {
	requestQuery := tx
	if lock {
		requestQuery = requestQuery.Clauses(clause.Locking{Strength: "UPDATE"})
	}
	var request model.ProjectSnapshotRequest
	if err := requestQuery.Where("project_id = ? AND target_seq = ? AND status = ?",
		projectID, targetSeq, model.SnapshotRequestPending).First(&request).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return model.ProjectSnapshotRequest{}, ErrForbidden
		}
		return model.ProjectSnapshotRequest{}, err
	}
	if request.AssignedMemberID == nil {
		return model.ProjectSnapshotRequest{}, ErrForbidden
	}
	memberQuery := tx
	if lock {
		memberQuery = memberQuery.Clauses(clause.Locking{Strength: "UPDATE"})
	}
	var member model.ProjectSessionMember
	if err := memberQuery.Where("id = ? AND session_id = ? AND user_id = ? AND device_id = ? AND desktop_session_id = ? AND left_at IS NULL",
		*request.AssignedMemberID, request.SessionID, actorUserID, deviceID, actorSessionID).
		First(&member).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return model.ProjectSnapshotRequest{}, ErrForbidden
		}
		return model.ProjectSnapshotRequest{}, err
	}
	return request, nil
}

// pendingSnapshotForUploadTx binds a live snapshot upload to the exact
// server-created request and the currently assigned host identity. Initial
// publication is the only snapshot flow without a live request.
func pendingSnapshotForUploadTx(tx *gorm.DB, project model.CloudProject,
	upload normalizedUpload, lock bool) (*model.ProjectSnapshotRequest, error) {
	if upload.Kind != "project_snapshot" {
		return nil, nil
	}
	if project.Status == model.ProjectUploading {
		return nil, nil
	}
	if project.Status != model.ProjectActive && project.Status != model.ProjectConflict {
		return nil, ErrProjectInactive
	}
	if upload.SnapshotSeq == nil || *upload.SnapshotSeq != project.HeadSeq {
		return nil, ErrConflict
	}
	request, err := pendingSnapshotRequestTx(tx, project.ID, *upload.SnapshotSeq,
		upload.ActorUserID, upload.DeviceID, upload.ActorSessionID, lock)
	if err != nil {
		return nil, err
	}
	return &request, nil
}

func retainLatestSnapshotsTx(tx *gorm.DB, projectID uuid.UUID, keep int) error {
	if keep < 1 || keep > 100 {
		return invalidf("snapshot retention is outside safe bounds")
	}
	oldSnapshots := `SELECT id FROM project_snapshots WHERE project_id = ?
		ORDER BY seq DESC, id DESC OFFSET ?`
	if err := tx.Exec(`DELETE FROM project_snapshot_requests
		WHERE completed_snapshot_id IN (`+oldSnapshots+`)`, projectID, keep).Error; err != nil {
		return err
	}
	return tx.Exec(`DELETE FROM project_snapshots
		WHERE project_id = ? AND id IN (
			`+oldSnapshots+`
		)`, projectID, projectID, keep).Error
}

func completeSnapshotRequestTx(tx *gorm.DB, request model.ProjectSnapshotRequest,
	snapshot model.ProjectSnapshot, now time.Time) (*SnapshotFinalization, error) {
	if request.TargetSeq != snapshot.Seq || request.ProjectID != snapshot.ProjectID {
		return nil, ErrConflict
	}
	result := tx.Model(&model.ProjectSnapshotRequest{}).
		Where("id = ? AND status = ?", request.ID, model.SnapshotRequestPending).
		Updates(map[string]any{
			"status": model.SnapshotRequestCompleted, "completed_snapshot_id": snapshot.ID,
			"completed_at": now,
		})
	if result.Error != nil {
		return nil, result.Error
	}
	if result.RowsAffected != 1 {
		return nil, ErrConflict
	}
	if request.Reason != model.SnapshotReasonSessionEnd {
		return nil, nil
	}
	var session model.ProjectSession
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("id = ? AND project_id = ? AND status = ?", request.SessionID,
			request.ProjectID, model.ProjectSessionEnding).First(&session).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return nil, ErrConflict
		}
		return nil, err
	}
	if err := (&Store{}).endSessionTx(tx, &session, now); err != nil {
		return nil, err
	}
	return &SnapshotFinalization{
		ProjectID: request.ProjectID, SessionID: request.SessionID, FinalSeq: snapshot.Seq,
	}, nil
}
