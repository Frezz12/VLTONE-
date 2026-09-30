package collab

import (
	"context"
	"encoding/json"
	"errors"
	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"sort"
	"strings"
	"time"
	"unicode/utf8"
	"vltstudio/backend/internal/model"
)

type editLeaseRow struct {
	ID             uuid.UUID `gorm:"primaryKey"`
	ProjectID      uuid.UUID
	SessionID      uuid.UUID
	HolderMemberID uuid.UUID
	FieldKeys      datatypes.JSON
	ExpiresAt      time.Time
}

func (editLeaseRow) TableName() string { return "project_edit_leases" }

type EditLease struct {
	LeaseID        uuid.UUID `json:"leaseId"`
	SessionID      uuid.UUID `json:"sessionId"`
	HolderMemberID uuid.UUID `json:"holderMemberId"`
	FieldKeys      []string  `json:"fieldKeys"`
	ExpiresAt      time.Time `json:"expiresAt"`
}
type EditLeaseHeldError struct {
	HolderMemberID uuid.UUID
	ExpiresAt      time.Time
}

func (e *EditLeaseHeldError) Error() string {
	return "this element is being edited by another participant"
}
func (e *EditLeaseHeldError) Unwrap() error { return ErrLeaseHeld }

type EditLeaseInput struct {
	FieldKeys              []string `json:"fieldKeys"`
	TTLSeconds             int      `json:"ttlSeconds"`
	ExpectedSessionVersion int64    `json:"expectedSessionVersion"`
}

func normalizeEditLeaseFields(fields []string) ([]string, error) {
	if len(fields) == 0 || len(fields) > 64 {
		return nil, invalidf("edit lease needs 1 to 64 fields")
	}
	result := append([]string{}, fields...)
	sort.Strings(result)
	for i, field := range result {
		parts := strings.Split(field, ":")
		if !utf8.ValidString(field) || len(field) > 512 || len(parts) < 2 || (i > 0 && result[i-1] == field) {
			return nil, invalidf("invalid edit lease field")
		}
		switch parts[0] {
		case "recording":
			if len(parts) != 2 {
				return nil, invalidf("recording lease must identify one participant")
			}
			if _, err := uuid.Parse(parts[1]); err != nil {
				return nil, invalidf("invalid recording participant")
			}
		case "clip", "plugin", "track", "send", "note", "take", "lane", "samplerFx":
			if _, err := uuid.Parse(parts[1]); err != nil {
				return nil, invalidf("invalid edit lease entity")
			}
		case "project":
			if len(parts) != 2 || parts[1] == "renderGeneration" || parts[1] == "" {
				return nil, invalidf("invalid project edit lease")
			}
		default:
			return nil, invalidf("unsupported edit lease entity")
		}
		for _, part := range parts {
			if part == "" || strings.ContainsAny(part, "\x00\r\n*") {
				return nil, invalidf("invalid edit lease field")
			}
		}
	}
	return result, nil
}
func editLeaseFieldsOverlap(a, b string) bool {
	return a == b || strings.HasPrefix(a, b+":") || strings.HasPrefix(b, a+":")
}
func editLeaseView(row editLeaseRow) EditLease {
	var fields []string
	_ = json.Unmarshal(row.FieldKeys, &fields)
	return EditLease{row.ID, row.SessionID, row.HolderMemberID, fields, row.ExpiresAt}
}

// Project row locking serializes acquisition with journal append on every API path.
func (s *Store) ChangeEditLease(ctx context.Context, projectID, sessionID, userID, deviceID, desktopID, leaseID uuid.UUID, method string, input EditLeaseInput) (EditLease, error) {
	var result EditLease
	if input.TTLSeconds == 0 {
		input.TTLSeconds = 15
	}
	if input.TTLSeconds < 1 || input.TTLSeconds > 30 {
		return result, invalidf("edit lease ttl must be 1 to 30 seconds")
	}
	var fields []string
	var err error
	if method == "acquire" {
		fields, err = normalizeEditLeaseFields(input.FieldKeys)
		if err != nil {
			return result, err
		}
	} else if len(input.FieldKeys) > 0 {
		return result, invalidf("renewal cannot change lease fields")
	}
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
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
		if session.CommandSchemaVersion < 6 {
			return ErrVersionMismatch
		}
		member, err := s.activeSessionMemberTx(tx, sessionID, userID, deviceID, desktopID, true)
		if err != nil {
			return err
		}
		for _, field := range fields {
			if strings.HasPrefix(field, "recording:") && field != "recording:"+member.ID.String() {
				return ErrForbidden
			}
		}
		now := s.now()
		var row editLeaseRow
		if method != "acquire" {
			if err := tx.First(&row, "id = ? AND session_id = ?", leaseID, sessionID).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					return ErrLeaseExpired
				}
				return err
			}
			if row.HolderMemberID != member.ID {
				return ErrForbidden
			}
			if method == "release" {
				return tx.Delete(&row).Error
			}
			if !row.ExpiresAt.After(now) {
				return ErrLeaseExpired
			}
		}
		if view.Project.Status != model.ProjectActive || !RoleAllows(view.Role, PermissionEdit) {
			return ErrForbidden
		}
		if err := SessionAllowsEdit(session, member); err != nil {
			return err
		}
		if input.ExpectedSessionVersion != 0 && input.ExpectedSessionVersion != session.Version {
			return ErrSessionVersion
		}
		if method == "acquire" {
			if err := s.enforceEditLeasesTx(tx, session, member.ID, fields); err != nil {
				return err
			}
			var count int64
			if err := tx.Model(&editLeaseRow{}).Where("session_id = ? AND holder_member_id = ? AND expires_at > ?", sessionID, member.ID, now).Count(&count).Error; err != nil {
				return err
			}
			if count >= 8 {
				return invalidf("too many active edit leases")
			}
			encoded, _ := json.Marshal(fields)
			row = editLeaseRow{ID: uuid.New(), ProjectID: projectID, SessionID: sessionID, HolderMemberID: member.ID, FieldKeys: datatypes.JSON(encoded)}
		} else if method != "renew" {
			return invalidf("unsupported lease action")
		}
		row.ExpiresAt = now.Add(time.Duration(input.TTLSeconds) * time.Second)
		if err := tx.Save(&row).Error; err != nil {
			return err
		}
		result = editLeaseView(row)
		return nil
	})
	return result, err
}

func (s *Store) enforceEditLeasesTx(tx *gorm.DB, session model.ProjectSession, memberID uuid.UUID, fields []string) error {
	if session.CommandSchemaVersion < 6 {
		return nil
	}
	now := s.now()
	// Expired and departed holders do not obstruct editing or reconnection.
	if err := tx.Where("session_id = ? AND (expires_at <= ? OR holder_member_id IN (SELECT id FROM project_session_members WHERE left_at IS NOT NULL))", session.ID, now).Delete(&editLeaseRow{}).Error; err != nil {
		return err
	}
	var rows []editLeaseRow
	if err := tx.Where("session_id = ? AND holder_member_id <> ? AND expires_at > ?", session.ID, memberID, now).Find(&rows).Error; err != nil {
		return err
	}
	for _, row := range rows {
		var holder model.ProjectSessionMember
		if err := tx.First(&holder, "id = ?", row.HolderMemberID).Error; err != nil {
			return err
		}
		if SessionAllowsEdit(session, holder) != nil {
			if err := tx.Delete(&row).Error; err != nil {
				return err
			}
			continue
		}
		lease := editLeaseView(row)
		for _, locked := range lease.FieldKeys {
			for _, field := range fields {
				if field != "project:renderGeneration" && editLeaseFieldsOverlap(locked, field) {
					return &EditLeaseHeldError{row.HolderMemberID, row.ExpiresAt}
				}
			}
		}
	}
	return nil
}
