package api

import (
	"context"
	"database/sql"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/migrations"
)

func (s *Server) collaborationEntitled(ctx context.Context,
	userID uuid.UUID) (bool, error) {
	var entitlement struct {
		CollaborationEnabled bool
	}
	err := s.DB.WithContext(ctx).Model(&model.User{}).
		Select("collaboration_enabled").Where("id = ?", userID).
		Take(&entitlement).Error
	if err != nil {
		return false, err
	}
	return entitlement.CollaborationEnabled || s.collabAllowedUsers[userID], nil
}

func (s *Server) requireCollaborationAccess(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !s.Config.CollaborationEnabled {
			writeError(w, r, http.StatusServiceUnavailable,
				"collaboration_not_enabled", "Cloud collaboration is not enabled.", nil)
			return
		}
		entitled, err := s.collaborationEntitled(r.Context(), userFrom(r).ID)
		if err != nil {
			writeError(w, r, http.StatusServiceUnavailable,
				"collaboration_access_unavailable",
				"Cloud collaboration access could not be checked.", nil)
			return
		}
		if !entitled {
			writeError(w, r, http.StatusForbidden, "collaboration_forbidden",
				"Cloud collaboration is not enabled for this account.", nil)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) desktopCapabilities(w http.ResponseWriter, r *http.Request) {
	entitled, err := s.collaborationEntitled(r.Context(), userFrom(r).ID)
	if err != nil {
		writeError(w, r, http.StatusServiceUnavailable,
			"collaboration_access_unavailable",
			"Cloud collaboration access could not be checked.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"collaboration": map[string]any{
			"enabled":         s.Config.CollaborationEnabled,
			"entitled":        entitled,
			"recording":       s.Config.CollabRecordingEnabled,
			"maxParticipants": s.Config.CollabMaxParticipants,
			"storage": map[string]any{
				"maximumObjectBytes": s.Config.CollabMaximumObjectBytes,
				"projectQuotaBytes":  s.Config.CollabProjectQuotaBytes,
				"userQuotaBytes":     s.Config.CollabUserQuotaBytes,
			},
		},
	})
}

func (s *Server) ready(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
	defer cancel()
	sqlDB, err := s.DB.DB()
	if err == nil {
		err = sqlDB.PingContext(ctx)
	}
	if err == nil {
		var present bool
		err = sqlDB.QueryRowContext(ctx,
			"SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version = $1)",
			migrations.LatestVersion()).Scan(&present)
		if errors.Is(err, sql.ErrNoRows) || err == nil && !present {
			err = errors.New("database schema is not current")
		}
	}
	if err == nil {
		directory := s.Config.StorageRoot
		if s.Config.CollaborationEnabled {
			directory = filepath.Join(directory, "collaboration-objects")
		}
		var file *os.File
		file, err = os.CreateTemp(directory, ".ready-*")
		if file != nil {
			name := file.Name()
			if closeErr := file.Close(); err == nil {
				err = closeErr
			}
			if removeErr := os.Remove(name); err == nil {
				err = removeErr
			}
		}
	}
	if err != nil {
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{"status": "not_ready"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "ready"})
}
