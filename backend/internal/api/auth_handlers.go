package api

import (
	"fmt"
	"log"
	"net/http"
	"net/mail"
	"net/smtp"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

type registerRequest struct {
	Email                string `json:"email"`
	Nickname             string `json:"nickname"`
	Password             string `json:"password"`
	PasswordConfirmation string `json:"password_confirmation"`
	Locale               string `json:"locale"`
	ConsentAccepted      bool   `json:"consent_accepted"`
	ConsentVersion       string `json:"consent_version"`
}

type loginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

func (s *Server) register(w http.ResponseWriter, r *http.Request) {
	now := time.Now().UTC()
	ip := requestIP(r)
	if !s.limiter.Allow("register-hour:"+ip, 3, time.Hour, now) ||
		!s.limiter.Allow("register-day:"+ip, 10, 24*time.Hour, now) {
		writeError(w, r, http.StatusTooManyRequests, "registration_rate_limited", "Too many registration attempts. Try again later.", nil)
		return
	}
	var input registerRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	fields := map[string]string{}
	emailKey := auth.NormalizeEmail(input.Email)
	nickname := strings.TrimSpace(input.Nickname)
	nicknameKey := auth.NormalizeNickname(nickname)
	if address, err := mail.ParseAddress(strings.TrimSpace(input.Email)); err != nil || auth.NormalizeEmail(address.Address) != emailKey {
		fields["email"] = "Enter a valid email address."
	}
	if count := len([]rune(nickname)); count < 3 || count > 32 {
		fields["nickname"] = "Nickname must contain between 3 and 32 characters."
	}
	if err := auth.ValidatePassword(input.Password); err != nil {
		fields["password"] = err.Error()
	}
	if input.Password != input.PasswordConfirmation {
		fields["password_confirmation"] = "Passwords do not match."
	}
	if input.Locale != "ru" && input.Locale != "en" {
		input.Locale = "en"
	}
	if !input.ConsentAccepted || input.ConsentVersion != s.Config.ConsentVersion {
		fields["consent_accepted"] = "The current diagnostics and privacy consent is required."
	}
	if len(fields) != 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	passwordHash, err := auth.HashPassword(input.Password)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "password_hash_failed", "Account could not be created.", nil)
		return
	}
	user := model.User{
		ID: uuid.New(), Email: strings.TrimSpace(input.Email), EmailKey: emailKey,
		Nickname: nickname, NicknameKey: nicknameKey, PasswordHash: passwordHash,
		Locale: input.Locale, Status: model.UserActive,
		ConsentVersion: input.ConsentVersion, ConsentAcceptedAt: now, ConsentIP: ip,
	}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(&user).Error; err != nil {
			return err
		}
		var plan model.Plan
		if err := tx.Where("code = ?", model.PlanDemo).First(&plan).Error; err != nil {
			return err
		}
		return tx.Create(&model.Subscription{
			ID: uuid.New(), UserID: user.ID, PlanID: plan.ID,
			Status: model.UserActive, StartsAt: now,
		}).Error
	})
	if err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "email_key") {
			fields["email"] = "An account with this email already exists."
		} else if strings.Contains(strings.ToLower(err.Error()), "nickname_key") {
			fields["nickname"] = "This nickname is already in use."
		} else {
			log.Printf("register: %v", err)
			writeError(w, r, http.StatusInternalServerError, "registration_failed", "Account could not be created.", nil)
			return
		}
		writeError(w, r, http.StatusConflict, "account_conflict", "This account cannot be created.", fields)
		return
	}
	session, raw, err := s.newWebSession(user.ID, now)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "session_failed", "Account was created, but sign in failed.", nil)
		return
	}
	s.setSessionCookie(w, webCookie, raw, session.ExpiresAt)
	s.writeUserSession(w, r, user, session.CSRFToken, http.StatusCreated)
}

func (s *Server) webLogin(w http.ResponseWriter, r *http.Request) {
	var input loginRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	now := time.Now().UTC()
	key := "web-login:" + requestIP(r) + ":" + auth.NormalizeEmail(input.Email)
	if !s.limiter.Allow(key, 5, 15*time.Minute, now) {
		writeError(w, r, http.StatusTooManyRequests, "login_rate_limited", "Too many sign-in attempts. Try again later.", nil)
		return
	}
	user, err := findUserByEmail(s.DB, input.Email)
	if err != nil || !auth.VerifyPassword(user.PasswordHash, input.Password) {
		writeError(w, r, http.StatusUnauthorized, "invalid_credentials", "Email or password is incorrect.", nil)
		return
	}
	if user.Status != model.UserActive {
		writeError(w, r, http.StatusForbidden, "account_suspended", "This account is suspended.", nil)
		return
	}
	session, raw, err := s.newWebSession(user.ID, now)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "session_failed", "Sign in failed.", nil)
		return
	}
	s.setSessionCookie(w, webCookie, raw, session.ExpiresAt)
	s.writeUserSession(w, r, user, session.CSRFToken, http.StatusOK)
}

func (s *Server) newWebSession(userID uuid.UUID, now time.Time) (model.WebSession, string, error) {
	raw, err := auth.RandomToken(32)
	if err != nil {
		return model.WebSession{}, "", err
	}
	csrf, err := auth.RandomToken(24)
	if err != nil {
		return model.WebSession{}, "", err
	}
	session := model.WebSession{
		ID: uuid.New(), UserID: userID, TokenHash: auth.HashToken(raw),
		CSRFToken: csrf, LastSeenAt: now, ExpiresAt: now.Add(30 * 24 * time.Hour),
	}
	return session, raw, s.DB.Create(&session).Error
}

func (s *Server) setSessionCookie(w http.ResponseWriter, name, value string, expires time.Time) {
	http.SetCookie(w, &http.Cookie{
		Name: name, Value: value, Path: "/", HttpOnly: true,
		Secure: s.Config.Environment == "production", SameSite: http.SameSiteLaxMode,
		Expires: expires, MaxAge: int(time.Until(expires).Seconds()),
	})
}

func (s *Server) clearSessionCookie(w http.ResponseWriter, name string) {
	http.SetCookie(w, &http.Cookie{
		Name: name, Value: "", Path: "/", HttpOnly: true,
		Secure: s.Config.Environment == "production", SameSite: http.SameSiteLaxMode,
		MaxAge: -1, Expires: time.Unix(1, 0),
	})
}

func (s *Server) writeUserSession(w http.ResponseWriter, r *http.Request, user model.User, csrf string, status int) {
	cycle, err := s.Quota.Current(user.ID, time.Now())
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "quota_unavailable", "Account quota is unavailable.", nil)
		return
	}
	writeJSON(w, status, map[string]any{
		"user": user, "csrf_token": csrf,
		"subscription": map[string]any{"plan": "demo", "display_name": "Demo", "all_features": true},
		"quota":        quotaResponse(cycle),
	})
}

func (s *Server) webLogout(w http.ResponseWriter, r *http.Request) {
	session := r.Context().Value(ctxWebSession).(model.WebSession)
	now := time.Now().UTC()
	s.DB.Model(&session).Update("revoked_at", now)
	s.clearSessionCookie(w, webCookie)
	w.WriteHeader(http.StatusNoContent)
}

type passwordResetRequestInput struct {
	Email string `json:"email"`
}

func (s *Server) passwordResetRequest(w http.ResponseWriter, r *http.Request) {
	var input passwordResetRequestInput
	if !decodeJSON(w, r, &input) {
		return
	}
	now := time.Now().UTC()
	key := "reset:" + requestIP(r) + ":" + auth.NormalizeEmail(input.Email)
	if !s.limiter.Allow(key, 3, time.Hour, now) {
		writeJSON(w, http.StatusAccepted, map[string]bool{"accepted": true})
		return
	}
	user, err := findUserByEmail(s.DB, input.Email)
	if err == nil && user.Status == model.UserActive {
		raw, randomErr := auth.RandomToken(32)
		if randomErr == nil {
			reset := model.PasswordResetToken{
				ID: uuid.New(), UserID: user.ID, TokenHash: auth.HashToken(raw),
				ExpiresAt: now.Add(30 * time.Minute),
			}
			if s.DB.Create(&reset).Error == nil {
				link := fmt.Sprintf("%s/%s/reset-password?token=%s", s.Config.PublicOrigin, user.Locale, raw)
				go s.sendPasswordReset(user.Email, link)
			}
		}
	}
	writeJSON(w, http.StatusAccepted, map[string]bool{"accepted": true})
}

type passwordResetConfirmInput struct {
	Token                string `json:"token"`
	Password             string `json:"password"`
	PasswordConfirmation string `json:"password_confirmation"`
}

func (s *Server) passwordResetConfirm(w http.ResponseWriter, r *http.Request) {
	var input passwordResetConfirmInput
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Password != input.PasswordConfirmation {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Passwords do not match.", map[string]string{"password_confirmation": "Passwords do not match."})
		return
	}
	hash, err := auth.HashPassword(input.Password)
	if err != nil {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", err.Error(), map[string]string{"password": err.Error()})
		return
	}
	now := time.Now().UTC()
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		var reset model.PasswordResetToken
		if err := tx.Where("token_hash = ? AND used_at IS NULL AND expires_at > ?", auth.HashToken(input.Token), now).First(&reset).Error; err != nil {
			return err
		}
		if err := tx.Model(&model.User{}).Where("id = ?", reset.UserID).Update("password_hash", hash).Error; err != nil {
			return err
		}
		if err := tx.Model(&reset).Update("used_at", now).Error; err != nil {
			return err
		}
		tx.Model(&model.WebSession{}).Where("user_id = ? AND revoked_at IS NULL", reset.UserID).Update("revoked_at", now)
		tx.Model(&model.DesktopSession{}).Where("user_id = ? AND revoked_at IS NULL", reset.UserID).Update("revoked_at", now)
		return nil
	})
	if err != nil {
		writeError(w, r, http.StatusBadRequest, "reset_token_invalid", "This password-reset link is invalid or expired.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) sendPasswordReset(to, link string) {
	if s.Config.SMTPHost == "" {
		if s.Config.Environment == "development" {
			log.Printf("development password reset for %s: %s", to, link)
		} else {
			log.Printf("password reset email was not sent: SMTP is unavailable")
		}
		return
	}
	if err := s.sendPlainEmail(to, "VLT Studio password reset",
		"Open this link within 30 minutes to choose a new password:\r\n"+link+"\r\n"); err != nil {
		log.Printf("send password reset: %v", err)
	}
}

func (s *Server) sendPlainEmail(to, subject, content string) error {
	from, err := mail.ParseAddress(s.Config.SMTPFrom)
	if err != nil {
		return fmt.Errorf("SMTP_FROM: %w", err)
	}
	recipient, err := mail.ParseAddress(to)
	if err != nil {
		return fmt.Errorf("recipient: %w", err)
	}
	hostPort := fmt.Sprintf("%s:%d", s.Config.SMTPHost, s.Config.SMTPPort)
	var smtpAuth smtp.Auth
	if s.Config.SMTPUsername != "" {
		smtpAuth = smtp.PlainAuth("", s.Config.SMTPUsername, s.Config.SMTPPassword, s.Config.SMTPHost)
	}
	body := "To: " + recipient.Address + "\r\nFrom: " + s.Config.SMTPFrom + "\r\n" +
		"Subject: " + subject + "\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n" + content
	return smtp.SendMail(hostPort, smtpAuth, from.Address, []string{recipient.Address}, []byte(body))
}
