package api

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
)

func TestSessionCompatibilityBodyIsRequiredAndCamelCase(t *testing.T) {
	emptyRequest := httptest.NewRequest(http.MethodPost, "/join", nil)
	emptyResponse := httptest.NewRecorder()
	var empty joinProjectSessionRequest
	if decodeSessionCompatibilityJSON(emptyResponse, emptyRequest, &empty) {
		t.Fatal("empty compatibility body was accepted")
	}
	if emptyResponse.Code != http.StatusUnprocessableEntity {
		t.Fatalf("empty compatibility status = %d", emptyResponse.Code)
	}
	var response APIError
	if err := json.Unmarshal(emptyResponse.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	if response.Code != "version_mismatch" {
		t.Fatalf("empty compatibility code = %q", response.Code)
	}

	validRequest := httptest.NewRequest(http.MethodPost, "/join", strings.NewReader(`{
		"appVersion":"1.2.3",
		"engineVersion":"engine-42",
		"commandSchemaVersion":2,
		"projectFormatVersion":7
	}`))
	validResponse := httptest.NewRecorder()
	var valid joinProjectSessionRequest
	if !decodeSessionCompatibilityJSON(validResponse, validRequest, &valid) {
		t.Fatalf("valid compatibility body was rejected: %s", validResponse.Body.String())
	}
	if valid.AppVersion != "1.2.3" || valid.EngineVersion != "engine-42" ||
		valid.CommandSchemaVersion != collab.CollaborationCommandSchemaVersion ||
		valid.ProjectFormatVersion != collab.CollaborationProjectFormatVersion {
		t.Fatalf("compatibility body decoded incorrectly: %#v", valid)
	}
}

func TestCollaborationJoinCloseReason(t *testing.T) {
	if got := collaborationJoinCloseReason(collab.ErrVersionMismatch); got != "incompatible collaboration client" {
		t.Fatalf("version mismatch close reason = %q", got)
	}
	if got := collaborationJoinCloseReason(collab.ErrForbidden); got != "session membership unavailable" {
		t.Fatalf("generic join close reason = %q", got)
	}
}

func TestCollaborationPresenceRejectsSensitiveDetailAndUnknownContent(t *testing.T) {
	coarseWithCoordinates := json.RawMessage(`{
		"surface":"settings","precision":"coarse","u":0.5,"v":0.5
	}`)
	if validateCollaborationPresence("presence.cursor", coarseWithCoordinates) == nil {
		t.Fatal("coarse settings presence accepted exact coordinates")
	}
	unknownFilename := json.RawMessage(`{
		"surface":"timeline","precision":"exact","u":0.5,"v":0.5,
		"filename":"private-session.wav"
	}`)
	if validateCollaborationPresence("presence.cursor", unknownFilename) == nil {
		t.Fatal("presence accepted a filename field")
	}
	unknownURL := json.RawMessage(`{
		"surface":"timeline","precision":"exact","targetId":"https://private.invalid"
	}`)
	if validateCollaborationPresence("presence.cursor", unknownURL) == nil {
		t.Fatal("presence accepted a URL as a semantic id")
	}
	safe := json.RawMessage(`{
		"surface":"timeline","precision":"exact",
		"trackId":"00000000-0000-0000-0000-000000000001",
		"timeSeconds":4.25,"u":0.2,"v":0.8
	}`)
	if err := validateCollaborationPresence("presence.cursor", safe); err != nil {
		t.Fatalf("safe presence was rejected: %v", err)
	}
}

func TestCollaborationEnvelopePreventsParticipantSpoofing(t *testing.T) {
	sequence := uint64(7)
	envelope := collaborationClientEnvelope{
		Protocol: collab.CollaborationProtocol, Type: "presence.cursor",
		MessageID: uuid.NewString(), ParticipantID: uuid.NewString(),
		EphemeralSeq: &sequence, SentAtMS: 1,
		Payload: json.RawMessage(`{"surface":"hidden","precision":"hidden"}`),
	}
	if validateCollaborationEnvelope(envelope, "", true) == nil {
		t.Fatal("client-selected participant identity was accepted")
	}
}

func TestCollaborationCommandRequiresLockedEightFieldShape(t *testing.T) {
	opID := uuid.NewString()
	valid := json.RawMessage(`{
		"schemaVersion":2,
		"opId":"` + opID + `",
		"transactionId":null,
		"baseServerSeq":0,
		"kind":"project.setScalar",
		"payload":{"field":"tempo","value":120},
		"preconditions":[],
		"touchedFields":["project:tempo"]
	}`)
	command, transactionID, err := parseCollaborationWireCommand(valid)
	if err != nil {
		t.Fatalf("valid command rejected: %v", err)
	}
	if command.OpID != opID || transactionID != nil {
		t.Fatal("valid command changed during parse")
	}
	var object map[string]any
	if err := json.Unmarshal(valid, &object); err != nil {
		t.Fatal(err)
	}
	object["localPath"] = "/Users/private/take.wav"
	withExtra, _ := json.Marshal(object)
	if _, _, err := parseCollaborationWireCommand(withExtra); err == nil {
		t.Fatal("command accepted an unlocked localPath sidecar")
	}
	delete(object, "localPath")
	delete(object, "transactionId")
	withoutRequired, _ := json.Marshal(object)
	if _, _, err := parseCollaborationWireCommand(withoutRequired); err == nil {
		t.Fatal("command accepted a missing locked field")
	}
}

func TestCollaborationRejectionTreatsMissingRecordingLeaseAsRetryable(t *testing.T) {
	code, _, retryable := collaborationRejection(collab.ErrLeaseRequired)
	if code != "lease_conflict" || !retryable {
		t.Fatalf("unexpected rejection: code=%q retryable=%v", code, retryable)
	}
	code, _, retryable = collaborationRejection(collab.ErrBaseSeqMismatch)
	if code != "sequence_gap" || !retryable {
		t.Fatalf("unexpected stale recording rejection: code=%q retryable=%v", code, retryable)
	}
}

func TestCollaborationRejectionClassifiesPermanentOperationFailures(t *testing.T) {
	tests := []struct {
		name        string
		err         error
		wantCode    string
		wantMessage string
	}{
		{
			name:        "operation id reused",
			err:         fmt.Errorf("append rejected: %w", collab.ErrOperationIDReuse),
			wantCode:    "operation_id_reused",
			wantMessage: "Operation identifier was reused with different content.",
		},
		{
			name:        "asset incomplete",
			err:         fmt.Errorf("append rejected: %w", collab.ErrAssetUnavailable),
			wantCode:    "asset_incomplete",
			wantMessage: "Complete and verify every referenced asset before committing the operation.",
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			code, message, retryable := collaborationRejection(test.err)
			if code != test.wantCode || message != test.wantMessage || retryable {
				t.Fatalf("unexpected rejection: code=%q message=%q retryable=%v", code, message, retryable)
			}
		})
	}
}

func TestProjectOperationStatusRejectsInvalidRouteIdentifiers(t *testing.T) {
	valid := uuid.NewString()
	tests := []struct {
		name      string
		projectID string
		opID      string
	}{
		{name: "project", projectID: "not-a-uuid", opID: valid},
		{name: "operation", projectID: valid, opID: "not-a-uuid"},
		{name: "nil operation", projectID: valid, opID: uuid.Nil.String()},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			route := chi.NewRouteContext()
			route.URLParams.Add("projectID", test.projectID)
			route.URLParams.Add("opID", test.opID)
			request := httptest.NewRequest(http.MethodGet, "/operation-status", nil)
			request = request.WithContext(context.WithValue(
				request.Context(), chi.RouteCtxKey, route))
			response := httptest.NewRecorder()

			(&Server{}).projectOperationStatus(response, request)

			if response.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
			}
			var apiError APIError
			if err := json.Unmarshal(response.Body.Bytes(), &apiError); err != nil {
				t.Fatal(err)
			}
			if apiError.Code != "invalid_id" {
				t.Fatalf("code = %q", apiError.Code)
			}
		})
	}
}

func TestCollaborationRESTUsesProtocolAssetIncompleteCode(t *testing.T) {
	tests := []struct {
		name  string
		write func(*Server, http.ResponseWriter, *http.Request, error)
	}{
		{name: "operation", write: func(server *Server, response http.ResponseWriter,
			request *http.Request, err error) {
			server.writeCollaborationError(response, request, err)
		}},
		{name: "storage", write: func(server *Server, response http.ResponseWriter,
			request *http.Request, err error) {
			server.writeCollaborationStorageError(response, request, err)
		}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			request := httptest.NewRequest(http.MethodPost, "/operation", nil)
			response := httptest.NewRecorder()
			test.write(&Server{}, response, request, collab.ErrAssetUnavailable)
			if response.Code != http.StatusConflict {
				t.Fatalf("status = %d", response.Code)
			}
			var apiError APIError
			if err := json.Unmarshal(response.Body.Bytes(), &apiError); err != nil {
				t.Fatal(err)
			}
			if apiError.Code != "asset_incomplete" {
				t.Fatalf("code = %q", apiError.Code)
			}
		})
	}
}

func TestCollaborationRuntimeLimitsFailClosed(t *testing.T) {
	if got := collaborationWSDBTimeout(0); got != 15*time.Second {
		t.Fatalf("default WS DB timeout = %s", got)
	}
	if got := collaborationWSDBTimeout(20); got != 20*time.Second {
		t.Fatalf("configured WS DB timeout = %s", got)
	}
	if got := normalizedRoomQueueBytes(1); got != collab.DefaultRoomQueueBytes {
		t.Fatalf("unsafe room byte budget = %d", got)
	}
	if got := normalizedRoomQueueBytes(16 << 20); got != 16<<20 {
		t.Fatalf("configured room byte budget = %d", got)
	}
}
