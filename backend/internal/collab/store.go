package collab

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
)

type Store struct {
	DB              *gorm.DB
	Now             func() time.Time
	MaxParticipants int
}

func NewStore(db *gorm.DB, maximumParticipants ...int) *Store {
	maximum := 8
	if len(maximumParticipants) != 0 && maximumParticipants[0] > 0 {
		maximum = maximumParticipants[0]
	}
	return &Store{DB: db, Now: func() time.Time { return time.Now().UTC() }, MaxParticipants: maximum}
}

func (s *Store) now() time.Time {
	if s.Now != nil {
		return s.Now().UTC()
	}
	return time.Now().UTC()
}

// lockActiveUsersTx establishes the global identity -> project lock order for
// transactions that will create or rewrite user foreign keys. Sorting avoids
// two multi-user mutations locking the same accounts in opposite order.
func lockActiveUsersTx(tx *gorm.DB, userIDs ...uuid.UUID) (map[uuid.UUID]model.User, error) {
	unique := make(map[uuid.UUID]struct{}, len(userIDs))
	for _, userID := range userIDs {
		if userID == uuid.Nil {
			return nil, gorm.ErrRecordNotFound
		}
		unique[userID] = struct{}{}
	}
	ids := make([]uuid.UUID, 0, len(unique))
	for userID := range unique {
		ids = append(ids, userID)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i].String() < ids[j].String() })
	var users []model.User
	if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
		Where("id IN ? AND status = ?", ids, model.UserActive).
		Order("id").Find(&users).Error; err != nil {
		return nil, err
	}
	if len(users) != len(ids) {
		return nil, gorm.ErrRecordNotFound
	}
	result := make(map[uuid.UUID]model.User, len(users))
	for _, user := range users {
		result[user.ID] = user
	}
	return result, nil
}

type ProjectView struct {
	Project model.CloudProject `json:"project"`
	Role    string             `json:"role"`
}

type CreateProjectInput struct {
	OwnerUserID       uuid.UUID
	Title             string
	FormatVersion     int
	EngineVersion     string
	MinimumAppVersion string
}

type UpdateProjectInput struct {
	Title             *string
	EngineVersion     *string
	MinimumAppVersion *string
}

type OwnershipTransfer struct {
	Project       model.CloudProject `json:"project"`
	PreviousOwner uuid.UUID          `json:"previousOwnerUserId"`
	NewOwner      uuid.UUID          `json:"newOwnerUserId"`
}

type Bootstrap struct {
	Project      model.CloudProject       `json:"project"`
	Role         string                   `json:"role"`
	Snapshot     *model.ProjectSnapshot   `json:"snapshot,omitempty"`
	Operations   []model.ProjectOperation `json:"operations"`
	FieldHeads   []model.ProjectFieldHead `json:"field_heads"`
	HeadSeq      int64                    `json:"head_seq"`
	NextAfterSeq int64                    `json:"next_after_seq"`
	HasMore      bool                     `json:"has_more"`
}

type OperationStatus struct {
	Found     bool                    `json:"found"`
	HeadSeq   int64                   `json:"head_seq"`
	Operation *model.ProjectOperation `json:"operation"`
}

type FieldConflict struct {
	FieldKey            string     `json:"fieldKey"`
	ExpectedOperationID uuid.UUID  `json:"expectedOperationId"`
	CurrentOperationID  *uuid.UUID `json:"currentOperationId,omitempty"`
	CurrentSeq          int64      `json:"currentServerSeq"`
}

type PreconditionError struct {
	Conflicts []FieldConflict
}

func (e *PreconditionError) Error() string { return "operation field precondition failed" }
func (e *PreconditionError) Unwrap() error { return ErrConflict }

func (s *Store) CreateProject(ctx context.Context, input CreateProjectInput) (ProjectView, error) {
	if input.OwnerUserID == uuid.Nil {
		return ProjectView{}, invalidf("owner is required")
	}
	title, err := validateProjectTitle(input.Title)
	if err != nil {
		return ProjectView{}, err
	}
	engineVersion, minimumVersion, err := validateCreateCompatibility(input.FormatVersion,
		input.EngineVersion, input.MinimumAppVersion)
	if err != nil {
		return ProjectView{}, err
	}
	now := s.now()
	project := model.CloudProject{
		ID: uuid.New(), OwnerUserID: input.OwnerUserID, Title: title,
		Status: model.ProjectUploading, FormatVersion: input.FormatVersion,
		EngineVersion: engineVersion, MinimumAppVersion: minimumVersion,
		CreatedAt: now, UpdatedAt: now,
	}
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if _, err := lockActiveUsersTx(tx, input.OwnerUserID); err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrForbidden
			}
			return err
		}
		return tx.Create(&project).Error
	})
	if err != nil {
		return ProjectView{}, err
	}
	return ProjectView{Project: project, Role: model.ProjectRoleOwner}, nil
}

func (s *Store) CompletePublication(ctx context.Context, projectID, actorUserID uuid.UUID) (ProjectView, error) {
	var result ProjectView
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionManageProject) {
			return ErrForbidden
		}
		if view.Project.Status == model.ProjectActive {
			result = view
			return nil
		}
		if view.Project.Status != model.ProjectUploading && view.Project.Status != model.ProjectConflict {
			return ErrConflict
		}
		var snapshot model.ProjectSnapshot
		if err := tx.Model(&model.ProjectSnapshot{}).
			Joins("JOIN blobs ON blobs.id = project_snapshots.blob_id").
			Where(`project_snapshots.project_id = ? AND project_snapshots.seq = ?
				AND project_snapshots.schema_version = ?
				AND blobs.status = ? AND blobs.kind = ?`,
				projectID, view.Project.HeadSeq, CollaborationProjectFormatVersion,
				BlobReady, "project_snapshot").
			Order("project_snapshots.seq DESC").First(&snapshot).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrConflict
			}
			return err
		}
		if err := requireSnapshotAssetsReadyTx(tx, projectID, snapshot.AssetIDs); err != nil {
			return err
		}
		// Publication is the atomic boundary after which collaborators may
		// bootstrap the canonical document. A prepared asset is part of that
		// publication attempt until it is either completed or explicitly
		// aborted/expired; otherwise a ready snapshot could become visible while
		// one of its assets is still only present in staging storage.
		var openAssetUploads int64
		if err := tx.Model(&model.UploadSession{}).
			Where("project_id = ? AND asset_id IS NOT NULL AND status IN ?", projectID,
				[]string{UploadPending, UploadUploading}).
			Count(&openAssetUploads).Error; err != nil {
			return err
		}
		if openAssetUploads != 0 {
			return ErrConflict
		}
		var unavailableAssets int64
		if err := tx.Model(&model.ProjectAsset{}).
			Joins("JOIN blobs ON blobs.id = project_assets.blob_id").
			Where(`project_assets.project_id = ? AND
				(blobs.status <> ? OR blobs.kind <> project_assets.kind)`,
				projectID, BlobReady).
			Count(&unavailableAssets).Error; err != nil {
			return err
		}
		if unavailableAssets != 0 {
			return ErrConflict
		}
		now := s.now()
		if err := tx.Model(&model.CloudProject{}).Where("id = ?", projectID).Updates(map[string]any{
			"status": model.ProjectActive, "snapshot_seq": snapshot.Seq, "updated_at": now,
		}).Error; err != nil {
			return err
		}
		view.Project.Status = model.ProjectActive
		view.Project.SnapshotSeq = snapshot.Seq
		view.Project.UpdatedAt = now
		result = view
		return nil
	})
	return result, err
}

func (s *Store) ListProjects(ctx context.Context, userID uuid.UUID) ([]ProjectView, error) {
	if userID == uuid.Nil {
		return nil, ErrForbidden
	}
	memberProjects := s.DB.WithContext(ctx).Model(&model.ProjectMember{}).
		Select("project_id").Where("user_id = ?", userID)
	var projects []model.CloudProject
	if err := s.DB.WithContext(ctx).
		Where("owner_user_id = ? OR id IN (?)", userID, memberProjects).
		Order("updated_at DESC, id").Find(&projects).Error; err != nil {
		return nil, err
	}
	var memberships []model.ProjectMember
	if err := s.DB.WithContext(ctx).Where("user_id = ?", userID).Find(&memberships).Error; err != nil {
		return nil, err
	}
	roles := make(map[uuid.UUID]string, len(memberships))
	for _, membership := range memberships {
		roles[membership.ProjectID] = membership.Role
	}
	views := make([]ProjectView, 0, len(projects))
	for _, project := range projects {
		role := roles[project.ID]
		if project.OwnerUserID == userID {
			role = model.ProjectRoleOwner
		}
		views = append(views, ProjectView{Project: project, Role: role})
	}
	return views, nil
}

func (s *Store) GetProject(ctx context.Context, projectID, userID uuid.UUID) (ProjectView, error) {
	return s.projectAccess(s.DB.WithContext(ctx), projectID, userID, false)
}

func (s *Store) UpdateProject(ctx context.Context, projectID, actorUserID uuid.UUID, input UpdateProjectInput) (ProjectView, error) {
	var view ProjectView
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		current, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(current.Role, PermissionManageProject) {
			return ErrForbidden
		}
		updates := map[string]any{"updated_at": s.now()}
		if input.Title != nil {
			title, err := validateProjectTitle(*input.Title)
			if err != nil {
				return err
			}
			updates["title"] = title
		}
		if input.EngineVersion != nil {
			value := strings.TrimSpace(*input.EngineVersion)
			if value == "" || !utf8.ValidString(value) || len(value) > 64 {
				return invalidf("engine version must contain between 1 and 64 bytes")
			}
			updates["engine_version"] = value
		}
		if input.MinimumAppVersion != nil {
			value := strings.TrimSpace(*input.MinimumAppVersion)
			if value == "" || !utf8.ValidString(value) || len(value) > 64 {
				return invalidf("minimum app version must contain between 1 and 64 bytes")
			}
			if _, ok := parseSemanticVersion(value); !ok {
				return invalidf("minimum app version must use semantic versioning")
			}
			updates["minimum_app_version"] = value
		}
		if _, changesEngine := updates["engine_version"]; changesEngine ||
			updates["minimum_app_version"] != nil {
			var liveSessions int64
			if err := tx.Model(&model.ProjectSession{}).
				Where("project_id = ? AND status IN ?", projectID,
					[]string{model.ProjectSessionStarting, model.ProjectSessionActive,
						model.ProjectSessionEnding}).
				Count(&liveSessions).Error; err != nil {
				return err
			}
			if liveSessions != 0 {
				return ErrConflict
			}
		}
		if len(updates) == 1 {
			view = current
			return nil
		}
		if err := tx.Model(&model.CloudProject{}).Where("id = ?", projectID).Updates(updates).Error; err != nil {
			return err
		}
		if err := tx.First(&current.Project, "id = ?", projectID).Error; err != nil {
			return err
		}
		view = current
		return nil
	})
	return view, err
}

func (s *Store) ArchiveProject(ctx context.Context, projectID, actorUserID uuid.UUID) error {
	return s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionManageProject) {
			return ErrForbidden
		}
		if view.Project.Status == model.ProjectArchived {
			return nil
		}
		var sessions []model.ProjectSession
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("project_id = ? AND status IN ?", projectID, []string{
				model.ProjectSessionStarting, model.ProjectSessionActive,
				model.ProjectSessionEnding,
			}).
			Find(&sessions).Error; err != nil {
			return err
		}
		if len(sessions) != 0 && view.Project.SnapshotSeq != view.Project.HeadSeq {
			// Archival must not bypass the exact-head final snapshot handshake.
			// The owner ends the live session first, then retries archive after the
			// assigned host's verified snapshot commits.
			return ErrConflict
		}
		now := s.now()
		if err := tx.Model(&model.CloudProject{}).Where("id = ?", projectID).Updates(map[string]any{
			"status": model.ProjectArchived, "archived_at": now, "updated_at": now,
		}).Error; err != nil {
			return err
		}
		for index := range sessions {
			if err := s.endSessionTx(tx, &sessions[index], now); err != nil {
				return err
			}
		}
		return nil
	})
}

func (s *Store) TransferOwnership(ctx context.Context, projectID, actorUserID, targetUserID uuid.UUID) (OwnershipTransfer, error) {
	if targetUserID == uuid.Nil || targetUserID == actorUserID {
		return OwnershipTransfer{}, invalidf("target owner must be another editor")
	}
	var result OwnershipTransfer
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if _, err := lockActiveUsersTx(tx, actorUserID, targetUserID); err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrForbidden
			}
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if view.Role != model.ProjectRoleOwner {
			return ErrForbidden
		}
		var target model.ProjectMember
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("project_id = ? AND user_id = ?", projectID, targetUserID).First(&target).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		if target.Role != model.ProjectRoleEditor {
			return ErrForbidden
		}
		var targetUser model.User
		if err := tx.Where("id = ? AND status = ?", targetUserID, model.UserActive).First(&targetUser).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrForbidden
			}
			return err
		}
		now := s.now()
		if err := tx.Model(&model.CloudProject{}).Where("id = ?", projectID).Updates(map[string]any{
			"owner_user_id": targetUserID, "updated_at": now,
		}).Error; err != nil {
			return err
		}
		if err := tx.Where("project_id = ? AND user_id = ?", projectID, targetUserID).
			Delete(&model.ProjectMember{}).Error; err != nil {
			return err
		}
		inviter := targetUserID
		previousOwner := model.ProjectMember{
			ProjectID: projectID, UserID: actorUserID, Role: model.ProjectRoleEditor,
			ColorIndex: target.ColorIndex, InvitedBy: &inviter, JoinedAt: now, UpdatedAt: now,
		}
		if err := tx.Create(&previousOwner).Error; err != nil {
			return err
		}
		view.Project.OwnerUserID = targetUserID
		view.Project.UpdatedAt = now
		result = OwnershipTransfer{
			Project: view.Project, PreviousOwner: actorUserID, NewOwner: targetUserID,
		}
		return nil
	})
	return result, err
}

func (s *Store) ListMembers(ctx context.Context, projectID, actorUserID uuid.UUID) ([]model.ProjectMember, error) {
	view, err := s.projectAccess(s.DB.WithContext(ctx), projectID, actorUserID, false)
	if err != nil {
		return nil, err
	}
	if !RoleAllows(view.Role, PermissionView) {
		return nil, ErrForbidden
	}
	var members []model.ProjectMember
	err = s.DB.WithContext(ctx).Where("project_id = ?", projectID).Order("joined_at, user_id").Find(&members).Error
	return members, err
}

func (s *Store) PutMember(ctx context.Context, projectID, actorUserID, memberUserID uuid.UUID, role string, colorIndex int16) (model.ProjectMember, error) {
	if !ValidProjectRole(role) || colorIndex < 0 || colorIndex > 31 || memberUserID == uuid.Nil {
		return model.ProjectMember{}, invalidf("member role, color or identifier is invalid")
	}
	var member model.ProjectMember
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if _, err := lockActiveUsersTx(tx, actorUserID, memberUserID); err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionManageMembers) {
			return ErrForbidden
		}
		if memberUserID == view.Project.OwnerUserID {
			return invalidf("the owner is not stored as a project member")
		}
		var user model.User
		if err := tx.Where("id = ? AND status = ?", memberUserID, model.UserActive).First(&user).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		now := s.now()
		inviter := actorUserID
		member = model.ProjectMember{
			ProjectID: projectID, UserID: memberUserID, Role: role, ColorIndex: colorIndex,
			InvitedBy: &inviter, JoinedAt: now, UpdatedAt: now,
		}
		if err := tx.Clauses(clause.OnConflict{
			Columns:   []clause.Column{{Name: "project_id"}, {Name: "user_id"}},
			DoUpdates: clause.Assignments(map[string]any{"role": role, "color_index": colorIndex, "updated_at": now}),
		}).Create(&member).Error; err != nil {
			return err
		}
		if role == model.ProjectRoleViewer {
			if err := tx.Exec(`DELETE FROM project_leases WHERE holder_member_id IN (
				SELECT members.id FROM project_session_members AS members
				JOIN project_live_sessions AS sessions ON sessions.id = members.session_id
				WHERE sessions.project_id = ? AND members.user_id = ? AND members.left_at IS NULL
			)`, projectID, memberUserID).Error; err != nil {
				return err
			}
			if err := s.reconcileProjectHostsTx(tx, view.Project, memberUserID, now); err != nil {
				return err
			}
		}
		return tx.First(&member, "project_id = ? AND user_id = ?", projectID, memberUserID).Error
	})
	return member, err
}

func (s *Store) RemoveMember(ctx context.Context, projectID, actorUserID, memberUserID uuid.UUID) error {
	return s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionManageMembers) {
			return ErrForbidden
		}
		now := s.now()
		if err := s.removeUserFromLiveSessionsTx(tx, view.Project, memberUserID, now); err != nil {
			return err
		}
		result := tx.Where("project_id = ? AND user_id = ?", projectID, memberUserID).Delete(&model.ProjectMember{})
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected == 0 {
			return ErrNotFound
		}
		return nil
	})
}

func (s *Store) Bootstrap(ctx context.Context, projectID, userID uuid.UUID, afterSeq int64, limit int) (Bootstrap, error) {
	if afterSeq < 0 {
		return Bootstrap{}, invalidf("after sequence cannot be negative")
	}
	limit = normalizeBootstrapLimit(limit)
	var result Bootstrap
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		view, err := s.projectAccess(tx, projectID, userID, false)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionView) {
			return ErrForbidden
		}
		if afterSeq > view.Project.HeadSeq {
			return ErrBaseSeqAhead
		}
		var snapshot model.ProjectSnapshot
		var snapshotPointer *model.ProjectSnapshot
		if err := tx.Model(&model.ProjectSnapshot{}).
			Joins("JOIN blobs ON blobs.id = project_snapshots.blob_id").
			Where("project_snapshots.project_id = ? AND project_snapshots.seq <= ? AND blobs.status = ? AND blobs.kind = ?",
				projectID, view.Project.SnapshotSeq, "ready", "project_snapshot").
			Order("project_snapshots.seq DESC").First(&snapshot).Error; err == nil {
			snapshotPointer = &snapshot
		} else if !errors.Is(err, gorm.ErrRecordNotFound) {
			return err
		}
		operationAfter := afterSeq
		if snapshotPointer != nil && snapshot.Seq > operationAfter {
			operationAfter = snapshot.Seq
		}
		var operations []model.ProjectOperation
		if err := tx.Where("project_id = ? AND seq > ? AND seq <= ?", projectID, operationAfter, view.Project.HeadSeq).
			Order("seq").Limit(limit + 1).Find(&operations).Error; err != nil {
			return err
		}
		hasMore := len(operations) > limit
		if hasMore {
			operations = operations[:limit]
		}
		next := operationAfter
		if len(operations) != 0 {
			next = operations[len(operations)-1].Seq
		}
		var heads []model.ProjectFieldHead
		if err := tx.Where("project_id = ?", projectID).Order("field_key").Find(&heads).Error; err != nil {
			return err
		}
		result = Bootstrap{
			Project: view.Project, Role: view.Role, Snapshot: snapshotPointer,
			Operations: operations, FieldHeads: heads, HeadSeq: view.Project.HeadSeq,
			NextAfterSeq: next, HasMore: hasMore,
		}
		return nil
	}, &sql.TxOptions{Isolation: sql.LevelRepeatableRead, ReadOnly: true})
	return result, err
}

// GetOperationStatus is the authoritative durability check for an operation
// identifier. The project head and optional operation are read from one
// repeatable-read snapshot so callers never compare results from different
// project revisions.
func (s *Store) GetOperationStatus(ctx context.Context, projectID, userID,
	operationID uuid.UUID) (OperationStatus, error) {
	if operationID == uuid.Nil {
		return OperationStatus{}, invalidf("operation identifier is required")
	}
	var result OperationStatus
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		view, err := s.projectAccess(tx, projectID, userID, false)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionView) {
			return ErrForbidden
		}
		result.HeadSeq = view.Project.HeadSeq

		var operation model.ProjectOperation
		lookup := tx.Where("project_id = ? AND op_id = ?", projectID, operationID).
			First(&operation)
		if errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
			result.Found = false
			result.Operation = nil
			return nil
		}
		if lookup.Error != nil {
			return lookup.Error
		}
		result.Found = true
		result.Operation = &operation
		return nil
	}, &sql.TxOptions{Isolation: sql.LevelRepeatableRead, ReadOnly: true})
	return result, err
}

func (s *Store) AppendOperation(ctx context.Context, input AppendOperationInput) (model.ProjectOperation, bool, error) {
	normalized, err := normalizeOperation(input)
	if err != nil {
		return model.ProjectOperation{}, false, err
	}
	var operation model.ProjectOperation
	duplicate := false
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, normalized.ActorUserID,
			normalized.ActorDeviceID, normalized.ActorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, normalized.ProjectID, normalized.ActorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionEdit) {
			return ErrForbidden
		}
		// Resolve an already-durable retry before checking ephemeral room state.
		// This preserves idempotency when the acknowledgement raced a disconnect;
		// only a genuinely new operation requires a live membership below.
		lookup := tx.Where("project_id = ? AND op_id = ?", normalized.ProjectID, normalized.OpID).First(&operation)
		if lookup.Error == nil {
			if operation.RequestHash != normalized.RequestHash || operation.ActorUserID == nil ||
				operation.ActorDeviceID == nil || *operation.ActorUserID != normalized.ActorUserID ||
				*operation.ActorDeviceID != normalized.ActorDeviceID {
				return ErrOperationIDReuse
			}
			duplicate = true
			return nil
		}
		if !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
			return lookup.Error
		}
		if view.Project.Status != model.ProjectActive {
			return ErrProjectInactive
		}
		member, err := s.requireActiveSessionMemberTx(tx, normalized.ProjectID,
			normalized.ActorUserID, normalized.ActorDeviceID,
			normalized.ActorSessionID)
		if err != nil {
			return err
		}
		if err := validateOperationBaseSeq(normalized.Kind, normalized.BaseSeq,
			view.Project.HeadSeq); err != nil {
			return err
		}
		if err := s.validateRecordingCommitRebaseTx(tx, normalized.ProjectID,
			normalized.Kind, normalized.BaseSeq, view.Project.HeadSeq,
			normalized.LeasePolicy.RecordingLeases); err != nil {
			return err
		}
		releaseLeaseIDs, err := s.enforceCommandLeasePolicyTx(tx, normalized.ProjectID,
			member, normalized.LeasePolicy)
		if err != nil {
			return err
		}
		if err := s.checkPreconditionsTx(tx, normalized.ProjectID, normalized.Preconditions); err != nil {
			return err
		}
		if err := s.checkLifecycleStepsTx(tx, normalized.ProjectID, normalized.OpID,
			normalized.LifecycleSteps); err != nil {
			return err
		}
		if err := requireCommandAssetsReadyTx(tx, normalized.ProjectID,
			normalized.Kind, normalized.Payload); err != nil {
			return err
		}
		preconditions, _ := json.Marshal(normalized.Preconditions)
		touchedFields, _ := json.Marshal(normalized.TouchedFields)
		now := s.now()
		actorUserID, actorDeviceID := normalized.ActorUserID, normalized.ActorDeviceID
		operation = model.ProjectOperation{
			ProjectID: normalized.ProjectID, Seq: view.Project.HeadSeq + 1, OpID: normalized.OpID,
			TransactionID: normalized.TransactionID,
			ActorUserID:   &actorUserID, ActorDeviceID: &actorDeviceID, Kind: normalized.Kind,
			SchemaVersion: normalized.SchemaVersion, BaseSeq: normalized.BaseSeq,
			Payload: datatypes.JSON(normalized.Payload), Preconditions: datatypes.JSON(preconditions),
			TouchedFields: datatypes.JSON(touchedFields), RequestHash: normalized.RequestHash, CreatedAt: now,
		}
		if err := tx.Create(&operation).Error; err != nil {
			return err
		}
		if err := tx.Model(&model.CloudProject{}).Where("id = ?", normalized.ProjectID).Updates(map[string]any{
			"head_seq": operation.Seq, "updated_at": now,
		}).Error; err != nil {
			return err
		}
		heads := make([]model.ProjectFieldHead, 0, len(normalized.TouchedFields))
		for _, fieldKey := range normalized.TouchedFields {
			heads = append(heads, model.ProjectFieldHead{
				ProjectID: normalized.ProjectID, FieldKey: fieldKey, HeadSeq: operation.Seq,
				HeadOpID: operation.OpID, UpdatedAt: now,
			})
		}
		if len(heads) != 0 {
			if err := tx.Clauses(clause.OnConflict{
				Columns:   []clause.Column{{Name: "project_id"}, {Name: "field_key"}},
				DoUpdates: clause.AssignmentColumns([]string{"head_seq", "head_op_id", "updated_at"}),
			}).CreateInBatches(&heads, 500).Error; err != nil {
				return err
			}
		}
		if len(releaseLeaseIDs) != 0 {
			deleted := tx.Where("project_id = ? AND session_id = ? AND id IN ?",
				normalized.ProjectID, member.SessionID, releaseLeaseIDs).
				Delete(&model.ProjectTrackLease{})
			if deleted.Error != nil {
				return deleted.Error
			}
			if deleted.RowsAffected != int64(len(releaseLeaseIDs)) {
				return ErrLeaseRequired
			}
		}
		return nil
	})
	return operation, duplicate, err
}

func validateOperationBaseSeq(_ string, baseSeq, headSeq int64) error {
	if baseSeq > headSeq {
		return ErrBaseSeqAhead
	}
	return nil
}

func (s *Store) validateRecordingCommitRebaseTx(tx *gorm.DB, projectID uuid.UUID,
	kind string, baseSeq, headSeq int64, leases []recordingLeaseReference) error {
	if kind != "recording.commit" || baseSeq == headSeq {
		return nil
	}
	if baseSeq > headSeq || len(leases) == 0 {
		return ErrBaseSeqMismatch
	}

	targetHeadKeys := recordingCommitTargetHeadKeys(leases)
	query := tx.Select("field_key", "head_seq").
		Where("project_id = ? AND head_seq > ? AND head_seq <= ?",
			projectID, baseSeq, headSeq).
		Where("field_key IN ?", targetHeadKeys)
	var heads []model.ProjectFieldHead
	if err := query.Find(&heads).Error; err != nil {
		return err
	}
	return validateRecordingCommitRebaseHeads(baseSeq, leases, heads)
}

func recordingCommitTargetHeadKeys(leases []recordingLeaseReference) []string {
	keys := make([]string, 0, len(leases)*2+1)
	keys = append(keys, recordingClipTrackAssignmentHead)
	for _, lease := range leases {
		prefix := "track:" + lease.TrackID.String() + ":"
		keys = append(keys, prefix+"clipLanding", prefix+"lifecycle")
	}
	sort.Strings(keys)
	return keys
}

func validateRecordingCommitRebaseHeads(baseSeq int64, leases []recordingLeaseReference,
	heads []model.ProjectFieldHead) error {
	targetTracks := make(map[string]struct{}, len(leases))
	for _, lease := range leases {
		targetTracks[lease.TrackID.String()] = struct{}{}
	}
	for _, head := range heads {
		if head.HeadSeq > baseSeq && recordingCommitRebaseSensitiveField(
			head.FieldKey, targetTracks) {
			return ErrBaseSeqMismatch
		}
	}
	return nil
}

func recordingCommitRebaseSensitiveField(fieldKey string,
	targetTrackIDs map[string]struct{}) bool {
	if fieldKey == recordingClipTrackAssignmentHead {
		return true
	}
	parts := strings.Split(fieldKey, ":")
	if len(parts) != 3 || parts[0] != "track" {
		return false
	}
	if parts[2] != "clipLanding" && parts[2] != "lifecycle" {
		return false
	}
	_, targeted := targetTrackIDs[parts[1]]
	return targeted
}

func (s *Store) requireActiveSessionMemberTx(tx *gorm.DB, projectID, userID,
	deviceID, desktopSessionID uuid.UUID) (model.ProjectSessionMember, error) {
	var session model.ProjectSession
	if err := tx.Where("project_id = ? AND status = ?", projectID, model.ProjectSessionActive).
		First(&session).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return model.ProjectSessionMember{}, ErrLiveSessionRequired
		}
		return model.ProjectSessionMember{}, err
	}
	member, err := s.activeSessionMemberTx(tx, session.ID, userID, deviceID,
		desktopSessionID, false)
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			return model.ProjectSessionMember{}, ErrLiveSessionRequired
		}
		return model.ProjectSessionMember{}, err
	}
	return member, nil
}

func (s *Store) enforceCommandLeasePolicyTx(tx *gorm.DB, projectID uuid.UUID,
	member model.ProjectSessionMember, policy commandLeasePolicy) ([]uuid.UUID, error) {
	if !policy.BlocksProjectTiming && len(policy.DeletedTrackIDs) == 0 &&
		len(policy.RecordingLeases) == 0 && len(policy.RoutedTrackIDs) == 0 {
		return nil, nil
	}
	trackIDs := append(append([]uuid.UUID(nil), policy.DeletedTrackIDs...),
		policy.RoutedTrackIDs...)
	for _, reference := range policy.RecordingLeases {
		trackIDs = append(trackIDs, reference.TrackID)
	}
	trackIDs = uniqueSortedUUIDs(trackIDs)
	query := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("project_id = ? AND lease_kind = ?",
			projectID, model.TrackLeaseRecord).Order("track_id, id")
	if !policy.BlocksProjectTiming {
		query = query.Where("track_id IN ?", trackIDs)
	}
	var leases []model.ProjectTrackLease
	if err := query.Find(&leases).Error; err != nil {
		return nil, err
	}
	return evaluateCommandLeasePolicy(policy, member, leases, s.now())
}

func evaluateCommandLeasePolicy(policy commandLeasePolicy, member model.ProjectSessionMember,
	leases []model.ProjectTrackLease, now time.Time) ([]uuid.UUID, error) {
	active := make([]model.ProjectTrackLease, 0, len(leases))
	for _, lease := range leases {
		if lease.LeaseKind == model.TrackLeaseRecord && lease.ExpiresAt.After(now) {
			active = append(active, lease)
		}
	}
	if policy.BlocksProjectTiming && len(active) != 0 {
		return nil, &LeaseHeldError{
			HolderMemberID: active[0].HolderMemberID, ExpiresAt: active[0].ExpiresAt,
		}
	}
	deletedTrackIDs := uniqueSortedUUIDs(policy.DeletedTrackIDs)
	routedTrackIDs := uniqueSortedUUIDs(policy.RoutedTrackIDs)
	blockedTracks := make(map[uuid.UUID]struct{}, len(deletedTrackIDs)+len(routedTrackIDs))
	for _, trackID := range deletedTrackIDs {
		blockedTracks[trackID] = struct{}{}
	}
	for _, trackID := range routedTrackIDs {
		blockedTracks[trackID] = struct{}{}
	}
	byTrack := make(map[uuid.UUID]model.ProjectTrackLease, len(active))
	for _, lease := range active {
		if _, blocked := blockedTracks[lease.TrackID]; blocked {
			return nil, &LeaseHeldError{
				HolderMemberID: lease.HolderMemberID, ExpiresAt: lease.ExpiresAt,
			}
		}
		byTrack[lease.TrackID] = lease
	}
	allByTrack := make(map[uuid.UUID]model.ProjectTrackLease, len(leases))
	for _, lease := range leases {
		if lease.LeaseKind == model.TrackLeaseRecord {
			allByTrack[lease.TrackID] = lease
		}
	}
	releaseIDs := make([]uuid.UUID, 0, len(policy.RecordingLeases))
	for _, reference := range policy.RecordingLeases {
		lease, exists := allByTrack[reference.TrackID]
		if !exists {
			return nil, ErrLeaseRequired
		}
		if lease.ID != reference.LeaseID {
			return nil, ErrLeaseRequired
		}
		if lease.HolderMemberID != member.ID {
			return nil, &LeaseHeldError{
				HolderMemberID: lease.HolderMemberID, ExpiresAt: lease.ExpiresAt,
			}
		}
		if lease.SessionID != member.SessionID {
			return nil, ErrLeaseRequired
		}
		if !lease.ExpiresAt.After(now) {
			return nil, ErrLeaseExpired
		}
		releaseIDs = append(releaseIDs, lease.ID)
	}
	sort.Slice(releaseIDs, func(i, j int) bool {
		return releaseIDs[i].String() < releaseIDs[j].String()
	})
	return releaseIDs, nil
}

func (s *Store) checkPreconditionsTx(tx *gorm.DB, projectID uuid.UUID, preconditions []FieldPrecondition) error {
	if len(preconditions) == 0 {
		return nil
	}
	keys := make([]string, 0, len(preconditions))
	for _, precondition := range preconditions {
		keys = append(keys, precondition.FieldKey)
	}
	var heads []model.ProjectFieldHead
	if err := tx.Where("project_id = ? AND field_key IN ?", projectID, keys).Find(&heads).Error; err != nil {
		return err
	}
	current := make(map[string]model.ProjectFieldHead, len(heads))
	for _, head := range heads {
		current[head.FieldKey] = head
	}
	var conflicts []FieldConflict
	for _, precondition := range preconditions {
		head, exists := current[precondition.FieldKey]
		if !exists || head.HeadOpID != precondition.OperationID {
			var currentOperationID *uuid.UUID
			if exists {
				value := head.HeadOpID
				currentOperationID = &value
			}
			conflicts = append(conflicts, FieldConflict{
				FieldKey: precondition.FieldKey, ExpectedOperationID: precondition.OperationID,
				CurrentOperationID: currentOperationID, CurrentSeq: head.HeadSeq,
			})
		}
	}
	if len(conflicts) != 0 {
		return &PreconditionError{Conflicts: conflicts}
	}
	return nil
}

type lifecycleState struct {
	Effect   lifecycleEffect
	HeadSeq  int64
	HeadOpID uuid.UUID
}

func (s *Store) checkLifecycleStepsTx(tx *gorm.DB, projectID, operationID uuid.UUID,
	steps []lifecycleStep) error {
	if len(steps) == 0 {
		return nil
	}
	keySet := make(map[string]struct{})
	for _, step := range steps {
		for _, requirement := range step.Requirements {
			keySet[requirement.FieldKey] = struct{}{}
		}
		for _, mutation := range step.Mutations {
			keySet[mutation.FieldKey] = struct{}{}
		}
	}
	keys := make([]string, 0, len(keySet))
	for key := range keySet {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	type lifecycleHead struct {
		FieldKey string
		HeadSeq  int64
		HeadOpID uuid.UUID
		Kind     string
		Payload  datatypes.JSON
	}
	var rows []lifecycleHead
	if err := tx.Table("project_field_heads AS heads").
		Select("heads.field_key, heads.head_seq, heads.head_op_id, ops.kind, ops.payload").
		Joins(`JOIN project_ops AS ops ON ops.project_id = heads.project_id
			AND ops.seq = heads.head_seq AND ops.op_id = heads.head_op_id`).
		Where("heads.project_id = ? AND heads.field_key IN ?", projectID, keys).Scan(&rows).Error; err != nil {
		return err
	}
	current := make(map[string]lifecycleState, len(rows))
	for _, row := range rows {
		effect, found, err := lifecycleEffectForOperation(row.Kind,
			json.RawMessage(row.Payload), row.FieldKey)
		if err != nil || !found {
			return ErrConflict
		}
		current[row.FieldKey] = lifecycleState{
			Effect: effect, HeadSeq: row.HeadSeq, HeadOpID: row.HeadOpID,
		}
	}
	return validateLifecycleSequence(current, operationID, steps)
}

func validateLifecycleSequence(current map[string]lifecycleState, operationID uuid.UUID,
	steps []lifecycleStep) error {
	for _, step := range steps {
		for _, requirement := range step.Requirements {
			head, exists := current[requirement.FieldKey]
			switch requirement.Kind {
			case lifecycleRequireLive:
				if exists && head.Effect == lifecycleDeleted {
					return ErrEntityDeleted
				}
			case lifecycleRequireVacant:
				if exists && head.Effect == lifecycleDeleted {
					return ErrEntityDeleted
				}
				if exists {
					return ErrConflict
				}
			case lifecycleRequireRestore:
				if requirement.ExpectedDeleteOperationID == nil {
					return ErrConflict
				}
				if !exists {
					return &PreconditionError{Conflicts: []FieldConflict{{
						FieldKey:            requirement.FieldKey,
						ExpectedOperationID: *requirement.ExpectedDeleteOperationID,
					}}}
				}
				if head.Effect != lifecycleDeleted || head.HeadOpID != *requirement.ExpectedDeleteOperationID {
					currentOperationID := head.HeadOpID
					return &PreconditionError{Conflicts: []FieldConflict{{
						FieldKey:            requirement.FieldKey,
						ExpectedOperationID: *requirement.ExpectedDeleteOperationID,
						CurrentOperationID:  &currentOperationID,
						CurrentSeq:          head.HeadSeq,
					}}}
				}
			default:
				return ErrConflict
			}
		}
		for _, mutation := range step.Mutations {
			current[mutation.FieldKey] = lifecycleState{
				Effect: mutation.Effect, HeadOpID: operationID,
			}
		}
	}
	return nil
}

func (s *Store) projectAccess(tx *gorm.DB, projectID, userID uuid.UUID, lock bool) (ProjectView, error) {
	if projectID == uuid.Nil || userID == uuid.Nil {
		return ProjectView{}, ErrNotFound
	}
	query := tx
	if lock {
		query = query.Clauses(clause.Locking{Strength: "UPDATE"})
	}
	var project model.CloudProject
	if err := query.First(&project, "id = ?", projectID).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return ProjectView{}, ErrNotFound
		}
		return ProjectView{}, err
	}
	role, err := s.roleForProjectTx(tx, project, userID)
	if err != nil {
		return ProjectView{}, err
	}
	return ProjectView{Project: project, Role: role}, nil
}

func (s *Store) roleForProjectTx(tx *gorm.DB, project model.CloudProject, userID uuid.UUID) (string, error) {
	if project.OwnerUserID == userID {
		return model.ProjectRoleOwner, nil
	}
	var member model.ProjectMember
	if err := tx.Where("project_id = ? AND user_id = ?", project.ID, userID).First(&member).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return "", ErrForbidden
		}
		return "", err
	}
	return member.Role, nil
}

func validateProjectTitle(value string) (string, error) {
	value = strings.TrimSpace(value)
	if !utf8.ValidString(value) || len([]rune(value)) < 1 || len([]rune(value)) > 160 {
		return "", invalidf("project title must contain between 1 and 160 characters")
	}
	return value, nil
}

func validateCreateCompatibility(formatVersion int, engineVersion, minimumAppVersion string) (string, string, error) {
	if formatVersion != CollaborationProjectFormatVersion {
		return "", "", invalidf("project format version must be %d", CollaborationProjectFormatVersion)
	}
	engineVersion = strings.TrimSpace(engineVersion)
	minimumAppVersion = strings.TrimSpace(minimumAppVersion)
	if engineVersion == "" || !utf8.ValidString(engineVersion) || len(engineVersion) > 64 {
		return "", "", invalidf("engine version must contain between 1 and 64 bytes")
	}
	if minimumAppVersion == "" || !utf8.ValidString(minimumAppVersion) || len(minimumAppVersion) > 64 {
		return "", "", invalidf("minimum app version must contain between 1 and 64 bytes")
	}
	if _, ok := parseSemanticVersion(minimumAppVersion); !ok {
		return "", "", invalidf("minimum app version must use semantic versioning")
	}
	return engineVersion, minimumAppVersion, nil
}
