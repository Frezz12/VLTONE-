package collab

import (
	"context"
	"errors"
	"net/mail"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

const (
	DefaultInviteTTL = 7 * 24 * time.Hour
	MinInviteTTL     = time.Hour
	MaxInviteTTL     = 30 * 24 * time.Hour
)

type CreateInviteInput struct {
	ProjectID      uuid.UUID
	ActorUserID    uuid.UUID
	ActorDeviceID  uuid.UUID
	ActorSessionID uuid.UUID
	Role           string
	TargetEmail    string
	TTL            time.Duration
}

type CreatedInvite struct {
	Invite model.ProjectInvite `json:"invite"`
	Token  string              `json:"token"`
}

func (s *Store) ListInvites(ctx context.Context, projectID,
	actorUserID uuid.UUID) ([]model.ProjectInvite, error) {
	view, err := s.projectAccess(s.DB.WithContext(ctx), projectID, actorUserID, false)
	if err != nil {
		return nil, err
	}
	if !RoleAllows(view.Role, PermissionManageMembers) {
		return nil, ErrForbidden
	}
	var invites []model.ProjectInvite
	err = s.DB.WithContext(ctx).Where("project_id = ?", projectID).
		Order("created_at DESC, id DESC").Limit(100).Find(&invites).Error
	return invites, err
}

func (s *Store) CreateInvite(ctx context.Context, input CreateInviteInput) (CreatedInvite, error) {
	if input.ProjectID == uuid.Nil || input.ActorUserID == uuid.Nil ||
		input.ActorDeviceID == uuid.Nil || input.ActorSessionID == uuid.Nil {
		return CreatedInvite{}, invalidf("project and exact desktop actor session are required")
	}
	if !ValidProjectRole(input.Role) {
		return CreatedInvite{}, invalidf("invite role must be editor or viewer")
	}
	if input.TTL == 0 {
		input.TTL = DefaultInviteTTL
	}
	if input.TTL < MinInviteTTL || input.TTL > MaxInviteTTL {
		return CreatedInvite{}, invalidf("invite expiry must be between one hour and thirty days")
	}
	var emailKey *string
	if value := strings.TrimSpace(input.TargetEmail); value != "" {
		address, err := mail.ParseAddress(value)
		if err != nil || !strings.EqualFold(strings.TrimSpace(address.Address), value) {
			return CreatedInvite{}, invalidf("target email is invalid")
		}
		normalized := auth.NormalizeEmail(address.Address)
		emailKey = &normalized
	}
	rawToken, err := auth.RandomToken(32)
	if err != nil {
		return CreatedInvite{}, err
	}
	now := s.now()
	actor := input.ActorUserID
	invite := model.ProjectInvite{
		ID: uuid.New(), ProjectID: input.ProjectID, InvitedBy: &actor, TargetEmailKey: emailKey,
		Role: input.Role, TokenHash: auth.HashToken(rawToken), ExpiresAt: now.Add(input.TTL), CreatedAt: now,
	}
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, input.ActorUserID,
			input.ActorDeviceID, input.ActorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, input.ProjectID, input.ActorUserID, true)
		if err != nil {
			return err
		}
		if !RoleAllows(view.Role, PermissionManageMembers) || view.Project.Status == model.ProjectArchived {
			return ErrForbidden
		}
		return tx.Create(&invite).Error
	})
	if err != nil {
		return CreatedInvite{}, err
	}
	return CreatedInvite{Invite: invite, Token: rawToken}, nil
}

func (s *Store) RevokeInvite(ctx context.Context, projectID, inviteID, actorUserID uuid.UUID) error {
	return s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if _, err := lockActiveUsersTx(tx, actorUserID); err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrForbidden
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
		var invite model.ProjectInvite
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND project_id = ?", inviteID, projectID).First(&invite).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		if invite.AcceptedAt != nil {
			return ErrInviteUsed
		}
		if invite.RevokedAt != nil {
			return nil
		}
		now := s.now()
		return tx.Model(&model.ProjectInvite{}).Where("id = ?", invite.ID).Update("revoked_at", now).Error
	})
}

func (s *Store) AcceptInvite(ctx context.Context, rawToken string, actorUserID uuid.UUID) (ProjectView, error) {
	rawToken = strings.TrimSpace(rawToken)
	if rawToken == "" || actorUserID == uuid.Nil {
		return ProjectView{}, invalidf("invite token is required")
	}
	var result ProjectView
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		users, err := lockActiveUsersTx(tx, actorUserID)
		if err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrForbidden
			}
			return err
		}
		user := users[actorUserID]
		var invite model.ProjectInvite
		tokenHash := auth.HashToken(rawToken)
		if err := tx.Where("token_hash = ?", tokenHash).First(&invite).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		var project model.CloudProject
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&project, "id = ?", invite.ProjectID).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		if project.Status == model.ProjectArchived {
			return ErrProjectInactive
		}
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND project_id = ? AND token_hash = ?", invite.ID, project.ID, tokenHash).
			First(&invite).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		now := s.now()
		if err := inviteAcceptanceError(invite, user, now); err != nil {
			return err
		}
		role := invite.Role
		if project.OwnerUserID == actorUserID {
			role = model.ProjectRoleOwner
		} else {
			var count int64
			if err := tx.Model(&model.ProjectMember{}).Where("project_id = ?", project.ID).Count(&count).Error; err != nil {
				return err
			}
			inviter := invite.InvitedBy
			member := model.ProjectMember{
				ProjectID: project.ID, UserID: actorUserID, Role: invite.Role,
				ColorIndex: int16(count % 32), InvitedBy: inviter, JoinedAt: now, UpdatedAt: now,
			}
			if err := tx.Clauses(clause.OnConflict{
				Columns:   []clause.Column{{Name: "project_id"}, {Name: "user_id"}},
				DoNothing: true,
			}).Create(&member).Error; err != nil {
				return err
			}
			if err := tx.Where("project_id = ? AND user_id = ?", project.ID, actorUserID).First(&member).Error; err != nil {
				return err
			}
			role = member.Role
		}
		if err := tx.Model(&model.ProjectInvite{}).Where("id = ?", invite.ID).Updates(map[string]any{
			"accepted_by": actorUserID, "accepted_at": now,
		}).Error; err != nil {
			return err
		}
		result = ProjectView{Project: project, Role: role}
		return nil
	})
	return result, err
}

func inviteAcceptanceError(invite model.ProjectInvite, user model.User, now time.Time) error {
	if invite.AcceptedAt != nil || invite.RevokedAt != nil {
		return ErrInviteUsed
	}
	if !invite.ExpiresAt.After(now) {
		return ErrInviteExpired
	}
	if invite.TargetEmailKey != nil && user.EmailKey != *invite.TargetEmailKey {
		return ErrForbidden
	}
	return nil
}
