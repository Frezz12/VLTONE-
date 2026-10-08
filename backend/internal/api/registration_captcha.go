package api

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"vltstudio/backend/internal/config"
)

var (
	errCaptchaInvalid        = errors.New("invalid registration captcha")
	errCaptchaUnavailable    = errors.New("registration captcha unavailable")
	defaultCaptchaHTTPClient = &http.Client{Timeout: 5 * time.Second, CheckRedirect: func(_ *http.Request, _ []*http.Request) error { return http.ErrUseLastResponse }}
)

func verifyRegistrationCaptcha(ctx context.Context, client *http.Client, cfg config.Config, token, ip string) error {
	if !cfg.RegistrationCaptchaRequired() {
		return nil
	}
	if cfg.TurnstileSiteKey == "" || cfg.TurnstileSecretKey == "" {
		return errCaptchaUnavailable
	}
	if cfg.Environment == "production" && (config.IsTurnstileTestKey(cfg.TurnstileSiteKey) || config.IsTurnstileTestKey(cfg.TurnstileSecretKey)) {
		return errCaptchaUnavailable
	}
	token = strings.TrimSpace(token)
	if token == "" || len(token) > 2048 {
		return errCaptchaInvalid
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	form := url.Values{"secret": {cfg.TurnstileSecretKey}, "response": {token}, "remoteip": {ip}}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, "https://challenges.cloudflare.com/turnstile/v0/siteverify", strings.NewReader(form.Encode()))
	if err != nil {
		return errCaptchaUnavailable
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	response, err := client.Do(req)
	if err != nil {
		return errCaptchaUnavailable
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return errCaptchaUnavailable
	}
	var result struct {
		Success  bool     `json:"success"`
		Hostname string   `json:"hostname"`
		Action   string   `json:"action"`
		Errors   []string `json:"error-codes"`
	}
	if json.NewDecoder(io.LimitReader(response.Body, 16<<10)).Decode(&result) != nil {
		return errCaptchaUnavailable
	}
	if !result.Success {
		for _, code := range result.Errors {
			if code == "internal-error" || code == "invalid-input-secret" || code == "missing-input-secret" {
				return errCaptchaUnavailable
			}
		}
		return errCaptchaInvalid
	}
	// Official dummy keys return synthetic action/hostname values. This exception
	// is restricted to explicit local test keys; production rejects those keys.
	if cfg.Environment != "production" && config.IsTurnstileTestKey(cfg.TurnstileSiteKey) && config.IsTurnstileTestKey(cfg.TurnstileSecretKey) {
		return nil
	}
	origin, err := url.Parse(cfg.PublicOrigin)
	if err != nil || origin.Hostname() == "" {
		return errCaptchaUnavailable
	}
	if result.Action != "register" || !strings.EqualFold(result.Hostname, origin.Hostname()) {
		return errCaptchaInvalid
	}
	return nil
}

func (s *Server) validateRegistrationCaptcha(w http.ResponseWriter, r *http.Request, token, ip string) bool {
	client := s.CaptchaHTTPClient
	if client == nil {
		client = defaultCaptchaHTTPClient
	}
	err := verifyRegistrationCaptcha(r.Context(), client, s.Config, token, ip)
	if errors.Is(err, errCaptchaUnavailable) {
		writeError(w, r, http.StatusServiceUnavailable, "captcha_unavailable", "Проверка безопасности временно недоступна. Попробуйте позже.", nil)
		return false
	}
	if err != nil {
		writeError(w, r, http.StatusUnprocessableEntity, "captcha_invalid", "Пройдите проверку безопасности ещё раз.", map[string]string{"captcha_token": "Подтвердите, что вы не робот."})
		return false
	}
	return true
}
