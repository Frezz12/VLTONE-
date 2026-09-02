package api

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
)

func TestCollaborationMetricsAreLoopbackOnlyAndAggregate(t *testing.T) {
	bus := collab.NewInProcessRoomBus()
	projectID := uuid.New()
	for range 2 {
		if _, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
			uuid.New(), 4, collab.DefaultRoomQueueBytes); err != nil {
			t.Fatal(err)
		}
	}
	bus.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: []byte("abc")})
	server := &Server{Rooms: bus, metrics: &collaborationMetrics{}}
	server.metrics.operations.Add(2)
	server.metrics.rejections.Add(1)

	external := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	external.RemoteAddr = "203.0.113.10:1234"
	externalResponse := httptest.NewRecorder()
	server.Router().ServeHTTP(externalResponse, external)
	if externalResponse.Code != http.StatusForbidden {
		t.Fatalf("external metrics status = %d, want 403", externalResponse.Code)
	}

	loopback := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	loopback.RemoteAddr = "127.0.0.1:1234"
	response := httptest.NewRecorder()
	server.Router().ServeHTTP(response, loopback)
	if response.Code != http.StatusOK {
		t.Fatalf("loopback metrics status = %d, want 200", response.Code)
	}
	if contentType := response.Header().Get("Content-Type"); contentType != "text/plain; version=0.0.4; charset=utf-8" {
		t.Fatalf("metrics content type = %q", contentType)
	}
	body := response.Body.String()
	for _, want := range []string{
		"# TYPE vlt_collaboration_rooms gauge\nvlt_collaboration_rooms 1\n",
		"vlt_collaboration_sockets 2\n",
		"vlt_collaboration_queued_bytes 6\n",
		"vlt_collaboration_object_cleanup_pending 0\n",
		"vlt_collaboration_object_cleanup_running 0\n",
		"# TYPE vlt_collaboration_operations_total counter\nvlt_collaboration_operations_total 2\n",
		"vlt_collaboration_rejections_total 1\n",
		"vlt_collaboration_gc_deletes_total 0\n",
		"vlt_collaboration_stuck_ending_sessions 0\n",
		"vlt_collaboration_snapshot_retries_total 0\n",
		"vlt_collaboration_object_cleanup_success_total 0\n",
		"vlt_collaboration_object_cleanup_failures_total 0\n",
	} {
		if !strings.Contains(body, want) {
			t.Fatalf("metrics body is missing %q:\n%s", want, body)
		}
	}
	if strings.Contains(body, projectID.String()) {
		t.Fatal("metrics exposed a project identifier")
	}
}
