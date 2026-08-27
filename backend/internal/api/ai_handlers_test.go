package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/google/uuid"

	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/model"
)

func TestProviderUsageAccounting(t *testing.T) {
	openAI := []byte(`{"usage":{"prompt_tokens":4,"completion_tokens":6,"total_tokens":10}}`)
	if got := extractUsage(openAI, "openai"); got != 10 {
		t.Fatalf("OpenAI usage = %d", got)
	}
	anthropic := []byte(`{"usage":{"input_tokens":10,"output_tokens":20,"cache_creation_input_tokens":30,"cache_read_input_tokens":40}}`)
	if got := extractUsage(anthropic, "anthropic"); got != 100 {
		t.Fatalf("Anthropic usage = %d", got)
	}
}

func TestAIProviderErrorMessage(t *testing.T) {
	for _, payload := range [][]byte{
		[]byte(`{"error":{"message":"unknown model id"}}`),
		[]byte(`{"message":"insufficient balance"}`),
	} {
		if got := aiProviderErrorMessage(payload); got == "" {
			t.Fatalf("provider error was lost: %s", payload)
		}
	}
	if got := aiProviderErrorMessage([]byte(`<html>gateway error</html>`)); got != "" {
		t.Fatalf("non-JSON provider body was exposed: %q", got)
	}
}

func TestStreamingUsageAccounting(t *testing.T) {
	for _, test := range []struct {
		provider string
		stream   string
		want     int64
	}{
		{"openai", "data: {\"usage\":{\"total_tokens\":42}}\n\ndata: [DONE]\n\n", 42},
		{"anthropic", "data: {\"message\":{\"usage\":{\"input_tokens\":10,\"cache_creation_input_tokens\":5}}}\n\ndata: {\"usage\":{\"output_tokens\":7,\"cache_read_input_tokens\":3}}\n\n", 25},
	} {
		recorder := httptest.NewRecorder()
		got, complete := proxySSE(recorder, strings.NewReader(test.stream), test.provider)
		if !complete || got != test.want || recorder.Body.String() != test.stream {
			t.Fatalf("%s SSE: usage=%d complete=%v body=%q", test.provider, got, complete, recorder.Body.String())
		}
	}
}

func TestAIKillSwitchFailsBeforeReadingAccount(t *testing.T) {
	server := &Server{Config: config.Config{AIEnabled: false}}
	recorder := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/v1/desktop/ai/models/00000000-0000-0000-0000-000000000001/invoke", strings.NewReader(`{"model":"ignored"}`))
	server.aiProxy(recorder, request)
	if recorder.Code != http.StatusServiceUnavailable || !strings.Contains(recorder.Body.String(), `"code":"ai_disabled"`) {
		t.Fatalf("kill switch response: %d %s", recorder.Code, recorder.Body.String())
	}
}

func TestAIModelSecretsRoundTripAndRejectTampering(t *testing.T) {
	server := &Server{Config: config.Config{SigningSeed: []byte("0123456789abcdef0123456789abcdef")}}
	ciphertext, err := server.encryptAISecret("provider-secret")
	if err != nil || strings.Contains(ciphertext, "provider-secret") {
		t.Fatalf("secret encryption failed: ciphertext=%q err=%v", ciphertext, err)
	}
	plain, err := server.decryptAISecret(ciphertext)
	if err != nil || plain != "provider-secret" {
		t.Fatalf("secret round trip = %q, %v", plain, err)
	}
	tamperedBytes := []byte(ciphertext)
	position := len(aiSecretVersion) + 4
	if tamperedBytes[position] == 'A' {
		tamperedBytes[position] = 'B'
	} else {
		tamperedBytes[position] = 'A'
	}
	tampered := string(tamperedBytes)
	if _, err := server.decryptAISecret(tampered); err == nil {
		t.Fatal("tampered AI credential was accepted")
	}
}

func TestDedicatedAIKeyCanReadLegacySigningSeedCredential(t *testing.T) {
	legacy := &Server{Config: config.Config{SigningSeed: []byte("0123456789abcdef0123456789abcdef")}}
	ciphertext, err := legacy.encryptAISecret("legacy-provider-secret")
	if err != nil {
		t.Fatal(err)
	}
	configured := &Server{Config: config.Config{
		SigningSeed:      []byte("0123456789abcdef0123456789abcdef"),
		AICredentialsKey: []byte("abcdef0123456789abcdef0123456789"),
	}}
	plain, err := configured.decryptAISecret(ciphertext)
	if err != nil || plain != "legacy-provider-secret" {
		t.Fatalf("legacy credential after dedicated key setup = %q, %v", plain, err)
	}
}

func TestResolveAIEndpointAcceptsBaseAndRequestURLs(t *testing.T) {
	for _, test := range []struct {
		name     string
		provider string
		raw      string
		want     string
	}{
		{"AnyModel base", "openai", "https://anymodel.org/v1", "https://anymodel.org/v1/chat/completions"},
		{"OpenAI origin", "openai", "https://api.openai.com", "https://api.openai.com/v1/chat/completions"},
		{"OpenAI final URL", "openai", "https://gateway.example/api/v1/chat/completions?region=eu", "https://gateway.example/api/v1/chat/completions?region=eu"},
		{"Anthropic base", "anthropic", "https://api.anthropic.com/v1/", "https://api.anthropic.com/v1/messages"},
		{"Anthropic final URL", "anthropic", "https://gateway.example/messages", "https://gateway.example/messages"},
	} {
		t.Run(test.name, func(t *testing.T) {
			got, err := resolveAIEndpoint(test.raw, test.provider)
			if err != nil || got != test.want {
				t.Fatalf("resolveAIEndpoint(%q, %q) = %q, %v; want %q", test.raw, test.provider, got, err, test.want)
			}
		})
	}
}

func TestAIModelPayloadsNeverExposeCredentialCiphertext(t *testing.T) {
	stored := model.AIModel{
		ID: uuid.New(), DisplayName: "Studio Assistant", Provider: "openai",
		ModelName: "gpt-4.1", EndpointURL: "https://provider.invalid/v1/chat/completions",
		APIKeyCiphertext: "v1:encrypted-secret", Enabled: true,
	}
	payload, err := json.Marshal(adminAIModel(stored))
	if err != nil {
		t.Fatal(err)
	}
	for _, forbidden := range []string{"encrypted-secret", "api_key_ciphertext"} {
		if strings.Contains(string(payload), forbidden) {
			t.Fatalf("admin AI model payload exposed %q: %s", forbidden, payload)
		}
	}
	if !strings.Contains(string(payload), `"has_api_key":true`) {
		t.Fatalf("admin AI model payload omitted safe key state: %s", payload)
	}
}

func TestAIModelValidationRequiresProviderModelEndpointAndKey(t *testing.T) {
	input := aiModelWritePayload{DisplayName: " ", Provider: "other", EndpointURL: "file:///tmp/model", APIKey: ""}
	fields := validateAIModel(&input, true, true)
	for _, name := range []string{"display_name", "provider", "model", "endpoint_url", "api_key"} {
		if fields[name] == "" {
			t.Fatalf("missing validation error for %s: %#v", name, fields)
		}
	}
}

func TestAILeaseValidationRejectsImpossibleReservations(t *testing.T) {
	server := &Server{}
	for _, body := range []string{
		`{"input_bytes":0,"max_output_tokens":100}`,
		`{"input_bytes":10,"max_output_tokens":0}`,
		`{"input_bytes":4194305,"max_output_tokens":100}`,
		`{"input_bytes":10,"max_output_tokens":1000001}`,
	} {
		recorder := httptest.NewRecorder()
		request := httptest.NewRequest(http.MethodPost, "/v1/desktop/ai/models/ignored/lease", strings.NewReader(body))
		server.leaseAIModel(recorder, request)
		if recorder.Code != http.StatusUnprocessableEntity || !strings.Contains(recorder.Body.String(), `"code":"ai_request_invalid"`) {
			t.Fatalf("invalid lease %s: %d %s", body, recorder.Code, recorder.Body.String())
		}
	}
}

func TestAISettlementOutcomesAreClosed(t *testing.T) {
	for _, outcome := range []string{"provider_usage", "provider_rejected", "transport_failure", "cancelled", "conservative_no_usage"} {
		if !validAISettlementOutcome(outcome) {
			t.Fatalf("valid outcome rejected: %s", outcome)
		}
	}
	for _, outcome := range []string{"", "success", "free", "provider_usage "} {
		if validAISettlementOutcome(outcome) {
			t.Fatalf("unknown outcome accepted: %q", outcome)
		}
	}
}
