package api

import (
	"encoding/json"
	"errors"
	"gorm.io/datatypes"
	"net/http"
	"slices"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

var errAdminMemberInvalid = errors.New("member unavailable or protected")

var adminPermissionCatalog = []string{
	"dashboard.read", "users.read", "users.write", "bugs.read", "bugs.write", "crashes.read",
	"releases.read", "releases.write", "backgrounds.read", "backgrounds.write",
	"models.read", "models.write", "prompts.read", "prompts.write", "audit.read",
	"tasks.read", "tasks.write",
}

func (s *Server) resolveAdminIdentity(admin *model.AdminUser) bool {
	if admin.UserID == nil {
		return admin.IsOwner
	}
	var user model.User
	if s.DB.First(&user, "id = ? AND status = ?", admin.UserID, model.UserActive).Error != nil {
		return false
	}
	admin.Email, admin.Nickname, admin.PasswordHash = user.Email, user.Nickname, user.PasswordHash
	return true
}

func adminCan(admin model.AdminUser, permission string) bool {
	return admin.IsOwner || slices.Contains(admin.Permissions, permission)
}

// A default-deny map protects every admin endpoint, including direct API calls.
func adminRoutePermission(method, path string) string {
	path = strings.TrimPrefix(path, "/v1/admin/")
	if path == "me" {
		return "session"
	}
	section := strings.Split(path, "/")[0]
	if section == "team" {
		return "owner"
	}
	if section == "ai" {
		parts := strings.Split(path, "/")
		if len(parts) < 2 {
			return "unknown"
		}
		section = parts[1]
	}
	if section == "browser-backgrounds" {
		section = "backgrounds"
	}
	permission := section + ".read"
	if method != http.MethodGet && method != http.MethodHead {
		permission = section + ".write"
	}
	if !slices.Contains(adminPermissionCatalog, permission) {
		return "unknown"
	}
	return permission
}

func (s *Server) adminPermissions(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		permission := adminRoutePermission(r.Method, r.URL.Path)
		admin := adminFrom(r)
		if permission != "session" && !admin.IsOwner && (permission == "owner" || !adminCan(admin, permission)) {
			writeError(w, r, 403, "permission_denied", "У вас нет прав на это действие.", nil)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) adminTeam(w http.ResponseWriter, r *http.Request) {
	members := []model.AdminUser{}
	if err := s.DB.Order("is_owner DESC, created_at ASC").Find(&members).Error; err != nil {
		writeError(w, r, 500, "team_unavailable", "Не удалось загрузить команду.", nil)
		return
	}
	for i := range members {
		s.resolveAdminIdentity(&members[i])
	}
	writeJSON(w, 200, map[string]any{"members": members, "permissions": adminPermissionCatalog})
}

func (s *Server) adminSaveMember(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Email       string   `json:"email"`
		Permissions []string `json:"permissions"`
		Enabled     *bool    `json:"enabled"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Enabled == nil || input.Permissions == nil || len(input.Permissions) > len(adminPermissionCatalog) || (*input.Enabled && len(input.Permissions) == 0) {
		writeError(w, r, 422, "invalid_member", "Укажите доступ и права.", nil)
		return
	}
	for _, permission := range input.Permissions {
		if !slices.Contains(adminPermissionCatalog, permission) || (strings.HasSuffix(permission, ".write") && !slices.Contains(input.Permissions, strings.TrimSuffix(permission, ".write")+".read")) {
			writeError(w, r, 422, "invalid_permissions", "Право изменения требует права просмотра раздела.", nil)
			return
		}
	}
	encodedPermissions, _ := json.Marshal(input.Permissions)
	var member model.AdminUser
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var user model.User
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).Where("email_key = ?", auth.NormalizeEmail(input.Email)).First(&user).Error; err != nil {
			return err
		}
		if user.Status != model.UserActive && *input.Enabled {
			return errAdminMemberInvalid
		}
		err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).Where("user_id = ? OR email_key = ?", user.ID, user.EmailKey).First(&member).Error
		if err != nil && !errors.Is(err, gorm.ErrRecordNotFound) {
			return err
		}
		if err == nil && (member.IsOwner || member.ID == adminFrom(r).ID || member.UserID == nil || *member.UserID != user.ID) {
			return errAdminMemberInvalid
		}
		status := model.UserSuspended
		if *input.Enabled {
			status = model.UserActive
		}
		if errors.Is(err, gorm.ErrRecordNotFound) {
			member = model.AdminUser{ID: uuid.New(), UserID: &user.ID, Email: user.Email, EmailKey: user.EmailKey, Nickname: user.Nickname, Status: status, Permissions: input.Permissions}
			if err := tx.Create(&member).Error; err != nil {
				return err
			}
		} else {
			if err := tx.Model(&member).Updates(map[string]any{"status": status, "permissions": datatypes.JSON(encodedPermissions), "email": user.Email, "email_key": user.EmailKey, "nickname": user.Nickname}).Error; err != nil {
				return err
			}
		}
		if err := tx.Model(&model.AdminSession{}).Where("admin_user_id = ? AND revoked_at IS NULL", member.ID).Update("revoked_at", time.Now().UTC()).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "admin.access.update", "admin", member.ID, map[string]any{"enabled": *input.Enabled, "permissions": input.Permissions})
	})
	if err != nil {
		if !errors.Is(err, gorm.ErrRecordNotFound) && !errors.Is(err, errAdminMemberInvalid) {
			writeError(w, r, 500, "team_unavailable", "Не удалось сохранить доступ. Повторите попытку.", nil)
			return
		}
		writeError(w, r, 422, "member_update_failed", "Проверьте email активного аккаунта сайта. Доступ владельца изменить нельзя.", nil)
		return
	}
	writeJSON(w, 200, map[string]any{"id": member.ID})
}
