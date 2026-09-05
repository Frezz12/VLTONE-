package collab

import (
	"context"
	"errors"
	"fmt"
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

	// Twelve digits is 10^12. Nine would have been friendlier to read aloud and
	// is defensible only because the accept endpoint already sits behind
	// desktopAuth and requireCollaborationAccess; twelve costs three keystrokes
	// and removes the question. The code is normally pasted from a link anyway.
	InviteCodeDigits = 12
	// Codes expire far sooner than the long token: they are meant for "join me
	// now", not for a link that sits in a chat for a month.
	DefaultCodeInviteTTL = 24 * time.Hour
	MaxCodeInviteTTL     = 7 * 24 * time.Hour

	inviteCodeDomain = "invite-code-v1"

	// Caps a targeted attack on one known-to-exist invite.
	MaxInviteAttempts   = 10
	InviteLockoutWindow = 15 * time.Minute
	// Caps the spray: guessing random codes hoping to hit any live invite.
	// Without these the per-invite counter is worthless, because the attacker
	// never touches the same invite twice. This is the load-bearing limit.
	AttemptWindow      = 15 * time.Minute
	MaxAttemptsPerUser = 20
	MaxAttemptsPerIP   = 60

	// Hit and miss must not be distinguishable by latency, so every redemption
	// by code takes at least this long regardless of outcome.
	inviteAttemptFloor = 120 * time.Millisecond
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
	// Shown once, exactly like the token. Never logged, never stored in clear.
	Code string `json:"code,omitempty"`
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
	// A code-bearing invite is short lived; the caller may still ask for less.
	// The long token on the same row expires with it, which is the point: one
	// row, one lifetime, and no way for the two halves to disagree.
	withCode := s.InviteCodesEnabled()
	maximumTTL := MaxInviteTTL
	if withCode {
		maximumTTL = MaxCodeInviteTTL
	}
	if input.TTL == 0 {
		if withCode {
			input.TTL = DefaultCodeInviteTTL
		} else {
			input.TTL = DefaultInviteTTL
		}
	}
	if input.TTL < MinInviteTTL || input.TTL > maximumTTL {
		return CreatedInvite{}, invalidf(
			"invite expiry must be between one hour and %d days",
			int(maximumTTL/(24*time.Hour)))
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
	var rawCode string
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
		if !withCode {
			return tx.Create(&invite).Error
		}
		// Retry only the unique-index collision. At 10^12 with the live invite
		// count this deployment can reach, a second collision is not a thing
		// that happens, but silently minting a duplicate would be.
		//
		// Each attempt is wrapped in a savepoint because Postgres aborts the
		// whole transaction on a constraint violation: without this, the retry
		// would fail with "current transaction is aborted" rather than trying
		// a second code.
		for attempt := range 5 {
			code, codeErr := generateInviteCode()
			if codeErr != nil {
				return codeErr
			}
			lookup, codeErr := s.inviteCodeLookup(code)
			if codeErr != nil {
				return codeErr
			}
			invite.CodeLookup = &lookup
			invite.CodeDigits = InviteCodeDigits
			savepoint := fmt.Sprintf("invite_code_%d", attempt)
			if spErr := tx.SavePoint(savepoint).Error; spErr != nil {
				return spErr
			}
			createErr := tx.Create(&invite).Error
			if createErr == nil {
				rawCode = code
				return nil
			}
			if !isUniqueViolation(createErr) {
				return createErr
			}
			if rbErr := tx.RollbackTo(savepoint).Error; rbErr != nil {
				return rbErr
			}
		}
		return errors.New("could not allocate a unique invite code")
	})
	if err != nil {
		return CreatedInvite{}, err
	}
	return CreatedInvite{Invite: invite, Token: rawToken, Code: rawCode}, nil
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

// AcceptInvite redeems a long token. It shares acceptInviteTx with the code
// path so the two can never drift on membership creation.
func (s *Store) AcceptInvite(ctx context.Context, rawToken string,
	actorUserID uuid.UUID, actorIPHash string) (ProjectView, error) {
	rawToken = strings.TrimSpace(rawToken)
	if rawToken == "" || actorUserID == uuid.Nil {
		return ProjectView{}, invalidf("invite token is required")
	}
	tokenHash := auth.HashToken(rawToken)
	return s.redeemInvite(ctx, actorUserID, actorIPHash, false,
		func(tx *gorm.DB) (model.ProjectInvite, error) {
			var invite model.ProjectInvite
			err := tx.Where("token_hash = ?", tokenHash).First(&invite).Error
			return invite, err
		},
		func(tx *gorm.DB, inviteID, projectID uuid.UUID) (model.ProjectInvite, error) {
			var invite model.ProjectInvite
			err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				Where("id = ? AND project_id = ? AND token_hash = ?",
					inviteID, projectID, tokenHash).
				First(&invite).Error
			return invite, err
		})
}

// AcceptInviteByCode redeems a short numeric code.
//
// Unlike the token path this one is genuinely guessable, so it is rate limited
// three ways and answers every failure identically. actorIPHash is a digest;
// the raw client address is never stored.
func (s *Store) AcceptInviteByCode(ctx context.Context, rawCode string,
	actorUserID uuid.UUID, actorIPHash string) (ProjectView, error) {
	// Every exit from here waits out the same floor, so a hit and a miss are
	// not distinguishable by how long the answer took.
	deadline := time.Now().Add(inviteAttemptFloor)
	defer func() {
		if remaining := time.Until(deadline); remaining > 0 {
			timer := time.NewTimer(remaining)
			defer timer.Stop()
			select {
			case <-timer.C:
			case <-ctx.Done():
			}
		}
	}()

	if !s.InviteCodesEnabled() {
		return ProjectView{}, ErrNotFound
	}
	if actorUserID == uuid.Nil {
		return ProjectView{}, invalidf("invite code is required")
	}
	code := NormalizeInviteCode(rawCode)
	if !ValidInviteCode(code) {
		// Still counts as an attempt: a malformed guess is a guess.
		_ = s.recordInviteAttempt(ctx, actorUserID, actorIPHash, nil, false)
		return ProjectView{}, ErrNotFound
	}
	lookup, err := s.inviteCodeLookup(code)
	if err != nil {
		return ProjectView{}, err
	}
	return s.redeemInvite(ctx, actorUserID, actorIPHash, true,
		func(tx *gorm.DB) (model.ProjectInvite, error) {
			var invite model.ProjectInvite
			err := tx.Where("code_lookup = ?", lookup).First(&invite).Error
			return invite, err
		},
		func(tx *gorm.DB, inviteID, projectID uuid.UUID) (model.ProjectInvite, error) {
			var invite model.ProjectInvite
			err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
				Where("id = ? AND project_id = ? AND code_lookup = ?",
					inviteID, projectID, lookup).
				First(&invite).Error
			return invite, err
		})
}

// redeemInvite is the shared body of both accept paths. The guessable flag
// turns on the per-invite attempt counter and lockout, which only make sense
// for a code.
func (s *Store) redeemInvite(ctx context.Context, actorUserID uuid.UUID,
	actorIPHash string, guessable bool,
	find func(tx *gorm.DB) (model.ProjectInvite, error),
	lock func(tx *gorm.DB, inviteID, projectID uuid.UUID) (model.ProjectInvite, error),
) (ProjectView, error) {
	if err := s.checkAttemptWindows(ctx, actorUserID, actorIPHash); err != nil {
		return ProjectView{}, err
	}
	var result ProjectView
	var attemptedInvite *uuid.UUID
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		users, err := lockActiveUsersTx(tx, actorUserID)
		if err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrForbidden
			}
			return err
		}
		user := users[actorUserID]
		invite, err := find(tx)
		if err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		inviteID := invite.ID
		attemptedInvite = &inviteID
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
		invite, err = lock(tx, invite.ID, project.ID)
		if err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		now := s.now()
		if guessable {
			if invite.LockedUntil != nil && invite.LockedUntil.After(now) {
				return ErrTooManyAttempts
			}
			if err := s.chargeInviteAttemptTx(tx, &invite, now); err != nil {
				return err
			}
		}
		if err := inviteAcceptanceError(invite, user, now); err != nil {
			return err
		}
		return s.acceptInviteTx(tx, invite, project, actorUserID, now, &result)
	})
	// Recorded outside the transaction so a rolled back redemption still leaves
	// the attempt behind. Failing to record must not mask the real outcome.
	_ = s.recordInviteAttempt(ctx, actorUserID, actorIPHash, attemptedInvite, err == nil)
	return result, err
}

// acceptInviteTx creates or reuses the membership and marks the invite spent.
// Shared verbatim by both accept paths — duplicating it is how the two would
// eventually grant different roles for the same invite.
func (s *Store) acceptInviteTx(tx *gorm.DB, invite model.ProjectInvite,
	project model.CloudProject, actorUserID uuid.UUID, now time.Time,
	result *ProjectView) error {
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
	// A spent code must stop being a lookup key immediately, so a later guess
	// of the same digits cannot even confirm that the code once existed.
	if invite.CodeLookup != nil {
		if err := tx.Model(&model.ProjectInvite{}).Where("id = ?", invite.ID).
			Update("code_lookup", nil).Error; err != nil {
			return err
		}
	}
	*result = ProjectView{Project: project, Role: role}
	return nil
}

// chargeInviteAttemptTx increments the per-invite counter and locks the invite
// out once it crosses the threshold. The charge lands before the acceptance
// checks so a wrong-email or expired guess still costs an attempt.
func (s *Store) chargeInviteAttemptTx(tx *gorm.DB, invite *model.ProjectInvite,
	now time.Time) error {
	invite.AttemptCount++
	updates := map[string]any{"attempt_count": invite.AttemptCount}
	if invite.AttemptCount >= MaxInviteAttempts {
		lockedUntil := now.Add(InviteLockoutWindow)
		invite.LockedUntil = &lockedUntil
		updates["locked_until"] = lockedUntil
	}
	return tx.Model(&model.ProjectInvite{}).Where("id = ?", invite.ID).
		Updates(updates).Error
}

// checkAttemptWindows enforces the per-account and per-IP sliding windows.
// These are what stop an attacker spraying random codes across many invites;
// the per-invite counter alone would never see the same row twice.
func (s *Store) checkAttemptWindows(ctx context.Context, actorUserID uuid.UUID,
	actorIPHash string) error {
	since := s.now().Add(-AttemptWindow)
	var userAttempts int64
	if err := s.DB.WithContext(ctx).Model(&model.InviteAttempt{}).
		Where("actor_user_id = ? AND attempted_at >= ?", actorUserID, since).
		Count(&userAttempts).Error; err != nil {
		return err
	}
	if userAttempts >= MaxAttemptsPerUser {
		return ErrTooManyAttempts
	}
	if actorIPHash == "" {
		return nil
	}
	var ipAttempts int64
	if err := s.DB.WithContext(ctx).Model(&model.InviteAttempt{}).
		Where("actor_ip_hash = ? AND attempted_at >= ?", actorIPHash, since).
		Count(&ipAttempts).Error; err != nil {
		return err
	}
	if ipAttempts >= MaxAttemptsPerIP {
		return ErrTooManyAttempts
	}
	return nil
}

func (s *Store) recordInviteAttempt(ctx context.Context, actorUserID uuid.UUID,
	actorIPHash string, inviteID *uuid.UUID, succeeded bool) error {
	if actorIPHash == "" {
		actorIPHash = strings.Repeat("0", 64)
	}
	return s.DB.WithContext(ctx).Create(&model.InviteAttempt{
		ID: uuid.New(), ActorUserID: actorUserID, ActorIPHash: actorIPHash,
		InviteID: inviteID, Succeeded: succeeded, AttemptedAt: s.now(),
	}).Error
}

// PurgeInviteAttempts drops attempt rows outside the window plus a margin, and
// releases the lookup key of invites that have expired. Called by maintenance.
func (s *Store) PurgeInviteAttempts(ctx context.Context) error {
	cutoff := s.now().Add(-24 * time.Hour)
	if err := s.DB.WithContext(ctx).
		Where("attempted_at < ?", cutoff).
		Delete(&model.InviteAttempt{}).Error; err != nil {
		return err
	}
	now := s.now()
	return s.DB.WithContext(ctx).Model(&model.ProjectInvite{}).
		Where("code_lookup IS NOT NULL AND expires_at < ?", now).
		Update("code_lookup", nil).Error
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
