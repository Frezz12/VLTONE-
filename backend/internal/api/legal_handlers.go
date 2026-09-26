package api

import (
	"errors"
	"net/http"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"vltstudio/backend/internal/legal"
	"vltstudio/backend/internal/model"
)

func validateRegistrationLegal(input registerRequest, version string) map[string]string {
	fields := map[string]string{}
	if !input.ConsentAccepted || input.ConsentVersion != version || version == "" {
		fields["consent_accepted"] = "Accept the current account data processing consent."
	}
	if !input.TermsAccepted || input.TermsVersion != version || version == "" {
		fields["terms_accepted"] = "Accept the current terms of use separately."
	}
	if input.DiagnosticsAccepted && input.DiagnosticsVersion != version {
		fields["diagnostics_accepted"] = "Review the current optional diagnostic consent."
	}
	return fields
}

func diagnosticsAllowed(user model.User) bool {
	return user.DiagnosticsConsentVersion == legal.Current.Version && user.DiagnosticsAcceptedAt != nil && user.DiagnosticsRevokedAt == nil
}

// Authentication loads the current user from the database on every request, so
// a previously issued desktop/reporter token cannot override a withdrawal.
func (s *Server) requireDiagnosticsConsent(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !diagnosticsAllowed(userFrom(r)) {
			writeError(w, r, http.StatusForbidden, "diagnostics_consent_required", "Optional diagnostics are disabled. You can change this in your account.", nil)
			return
		}
		next.ServeHTTP(w, r)
	})
}

var errDiagnosticsConsentRequired = errors.New("optional diagnostic consent is required")

// Recheck in the ingestion transaction so a withdrawal and an in-flight upload
// are ordered. Reuse its connection instead of holding a second pool slot.
func lockDiagnosticConsent(tx *gorm.DB, userID uuid.UUID) error {
	var current model.User
	if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).First(&current, "id = ?", userID).Error; err != nil {
		return err
	}
	if !diagnosticsAllowed(current) {
		return errDiagnosticsConsentRequired
	}
	return nil
}

func (s *Server) updateDiagnosticsConsent(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Accepted *bool  `json:"accepted"`
		Version  string `json:"version"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Accepted == nil || (*input.Accepted && (input.Version != legal.Current.Version || !s.Config.LegalProfile.Ready())) {
		writeError(w, r, http.StatusUnprocessableEntity, "diagnostics_consent_invalid", "Review the current diagnostic consent before enabling it.", nil)
		return
	}
	user := userFrom(r)
	err := s.DB.WithContext(r.Context()).Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&user, "id = ?", user.ID).Error; err != nil {
			return err
		}
		now := time.Now().UTC()
		if (*input.Accepted && diagnosticsAllowed(user)) || (!*input.Accepted && user.DiagnosticsRevokedAt != nil) {
			return nil
		}
		action := "revoked"
		updates := map[string]any{"diagnostics_revoked_at": now}
		version := user.DiagnosticsConsentVersion
		if version == "" {
			version = user.ConsentVersion
		}
		if *input.Accepted {
			action = "accepted"
			version = input.Version
			updates = map[string]any{"diagnostics_consent_version": version, "diagnostics_accepted_at": now, "diagnostics_revoked_at": nil}
		}
		if err := tx.Model(&user).Updates(updates).Error; err != nil {
			return err
		}
		return tx.Create(&model.LegalAcceptance{ID: uuid.New(), UserID: user.ID, Purpose: "diagnostics", Version: version, Action: action, OccurredAt: now, IP: requestIP(r)}).Error
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "diagnostics_consent_failed", "Your diagnostic preference could not be saved.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"enabled": diagnosticsAllowed(user)})
}
