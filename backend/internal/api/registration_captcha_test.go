package api

import (
	"context"
	"errors"
	"io"
	"net/http"
	"strings"
	"testing"

	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/model"
)

type captchaTransport func(*http.Request) (*http.Response, error)

func (fn captchaTransport) RoundTrip(r *http.Request) (*http.Response, error) { return fn(r) }

func TestRegistrationCaptchaVerification(t *testing.T) {
	cfg := config.Config{Environment: "production", PublicOrigin: "https://vltstudio.ru", TurnstileSiteKey: "public-unit-key", TurnstileSecretKey: "private-unit-key"}
	for _, check := range []struct {
		name, body string
		status     int
		want       error
	}{
		{"valid", `{"success":true,"hostname":"vltstudio.ru","action":"register"}`, 200, nil},
		{"replayed", `{"success":false,"error-codes":["timeout-or-duplicate"]}`, 200, errCaptchaInvalid},
		{"rejected", `{"success":false}`, 200, errCaptchaInvalid},
		{"foreign host", `{"success":true,"hostname":"elsewhere.example","action":"register"}`, 200, errCaptchaInvalid},
		{"foreign action", `{"success":true,"hostname":"vltstudio.ru","action":"login"}`, 200, errCaptchaInvalid},
		{"missing action", `{"success":true,"hostname":"vltstudio.ru"}`, 200, errCaptchaInvalid},
		{"provider outage", `{"success":true}`, 503, errCaptchaUnavailable},
		{"invalid secret", `{"success":false,"error-codes":["invalid-input-secret"]}`, 200, errCaptchaUnavailable},
		{"invalid response", `not json`, 200, errCaptchaUnavailable},
	} {
		t.Run(check.name, func(t *testing.T) {
			client := &http.Client{Transport: captchaTransport(func(r *http.Request) (*http.Response, error) {
				if r.Method != http.MethodPost || r.URL.String() != "https://challenges.cloudflare.com/turnstile/v0/siteverify" {
					t.Fatalf("wrong verification request: %s %s", r.Method, r.URL)
				}
				_ = r.ParseForm()
				if r.Form.Get("secret") != cfg.TurnstileSecretKey || r.Form.Get("response") != "one-use-token" || r.Form.Get("remoteip") != "203.0.113.10" {
					t.Fatal("verification fields missing")
				}
				return &http.Response{StatusCode: check.status, Body: io.NopCloser(strings.NewReader(check.body)), Header: http.Header{}}, nil
			})}
			if got := verifyRegistrationCaptcha(context.Background(), client, cfg, "one-use-token", "203.0.113.10"); !errors.Is(got, check.want) {
				t.Fatalf("verification error = %v, want %v", got, check.want)
			}
		})
	}
	client := &http.Client{Transport: captchaTransport(func(_ *http.Request) (*http.Response, error) {
		t.Fatal("invalid local request reached provider")
		return nil, errors.New("unexpected")
	})}
	for _, token := range []string{"", strings.Repeat("x", 2049)} {
		if got := verifyRegistrationCaptcha(context.Background(), client, cfg, token, ""); !errors.Is(got, errCaptchaInvalid) {
			t.Fatalf("invalid token accepted: %v", got)
		}
	}
	missing := cfg
	missing.TurnstileSecretKey = ""
	if got := verifyRegistrationCaptcha(context.Background(), client, missing, "token", ""); !errors.Is(got, errCaptchaUnavailable) {
		t.Fatalf("missing configuration accepted: %v", got)
	}
	testKeys := cfg
	testKeys.TurnstileSiteKey = "1x00000000000000000000AA"
	if got := verifyRegistrationCaptcha(context.Background(), client, testKeys, "token", ""); !errors.Is(got, errCaptchaUnavailable) {
		t.Fatal("test key accepted in production")
	}
	if got := verifyRegistrationCaptcha(context.Background(), client, config.Config{Environment: "development"}, "", ""); got != nil {
		t.Fatalf("unconfigured local registration was blocked: %v", got)
	}
	broken := &http.Client{Transport: captchaTransport(func(_ *http.Request) (*http.Response, error) { return nil, context.DeadlineExceeded })}
	if got := verifyRegistrationCaptcha(context.Background(), broken, cfg, "token", ""); !errors.Is(got, errCaptchaUnavailable) {
		t.Fatalf("network error was ignored: %v", got)
	}
}

func checkRegistrationCaptcha(t *testing.T, s *Server, router http.Handler, input map[string]any) testResponse {
	t.Helper()
	originalConfig, originalClient := s.Config, s.CaptchaHTTPClient
	defer func() { s.Config = originalConfig; s.CaptchaHTTPClient = originalClient }()
	s.Config.TurnstileSiteKey, s.Config.TurnstileSecretKey = "public-integration-key", "private-integration-key"
	s.CaptchaHTTPClient = &http.Client{Transport: captchaTransport(func(r *http.Request) (*http.Response, error) {
		_ = r.ParseForm()
		body := `{"success":false,"error-codes":["timeout-or-duplicate"]}`
		if r.Form.Get("response") == "valid-integration-token" {
			body = `{"success":true,"hostname":"localhost","action":"register"}`
		}
		return &http.Response{StatusCode: 200, Body: io.NopCloser(strings.NewReader(body)), Header: http.Header{}}, nil
	})}
	meta := performJSON(router, http.MethodGet, "/v1/meta", nil, "203.0.113.10:1234", nil, nil)
	if meta.Body["registration_captcha"].(map[string]any)["site_key"] != s.Config.TurnstileSiteKey || strings.Contains(string(jsonBytes(meta.Body)), s.Config.TurnstileSecretKey) {
		t.Fatal("public CAPTCHA metadata incorrect or contains a secret")
	}
	var before int64
	s.DB.Model(&model.User{}).Count(&before)
	for _, token := range []string{"", "expired-integration-token"} {
		payload := map[string]any{}
		for key, value := range input {
			payload[key] = value
		}
		payload["captcha_token"] = token
		result := performJSON(router, http.MethodPost, "/v1/web/auth/register", payload, "203.0.113.42:1234", nil, nil)
		if result.Status != http.StatusUnprocessableEntity || result.Body["code"] != "captcha_invalid" || len(result.Cookies) != 0 {
			t.Fatalf("invalid CAPTCHA registered an account: %d %v", result.Status, result.Body)
		}
	}
	var after int64
	s.DB.Model(&model.User{}).Count(&after)
	if after != before {
		t.Fatal("invalid CAPTCHA created a database record")
	}
	payload := map[string]any{}
	for key, value := range input {
		payload[key] = value
	}
	payload["captcha_token"] = "valid-integration-token"
	return performJSON(router, http.MethodPost, "/v1/web/auth/register", payload, "203.0.113.10:1234", nil, nil)
}
