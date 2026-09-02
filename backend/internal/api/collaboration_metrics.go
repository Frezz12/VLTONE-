package api

import (
	"fmt"
	"log"
	"net"
	"net/http"
	"strings"
	"sync/atomic"

	"vltstudio/backend/internal/collab"
)

type collaborationMetrics struct {
	operations                atomic.Uint64
	rejections                atomic.Uint64
	resyncs                   atomic.Uint64
	hashRounds                atomic.Uint64
	hashTimeouts              atomic.Uint64
	uploadVerificationSuccess atomic.Uint64
	uploadVerificationFailure atomic.Uint64
	snapshotRequests          atomic.Uint64
	snapshotRetries           atomic.Uint64
	sessionEnding             atomic.Uint64
	sessionEnded              atomic.Uint64
	stuckEndingSessions       atomic.Int64
	maintenanceFailures       atomic.Uint64
	storageFailures           atomic.Uint64
	gcFailures                atomic.Uint64
	gcDeletes                 atomic.Uint64
	objectCleanupSuccess      atomic.Uint64
	objectCleanupFailures     atomic.Uint64
	objectCleanupPending      atomic.Int64
	objectCleanupRunning      atomic.Int64
}

func (s *Server) collaborationMetrics(w http.ResponseWriter, r *http.Request) {
	peer := net.ParseIP(requestIP(r))
	if peer == nil || !peer.IsLoopback() {
		http.Error(w, "metrics are available on loopback only", http.StatusForbidden)
		return
	}
	var rooms collab.RoomBusStats
	if s.Rooms != nil {
		rooms = s.Rooms.Stats()
	}
	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = fmt.Fprint(w, s.metricsText(rooms))
}

func (s *Server) metricsText(rooms collab.RoomBusStats) string {
	metrics := s.metrics
	if metrics == nil {
		metrics = &collaborationMetrics{}
	}
	var output strings.Builder
	writeGauge := func(name, help string, value int64) {
		fmt.Fprintf(&output, "# HELP %s %s\n# TYPE %s gauge\n%s %d\n",
			name, help, name, name, value)
	}
	writeCounter := func(name, help string, value uint64) {
		fmt.Fprintf(&output, "# HELP %s %s\n# TYPE %s counter\n%s %d\n",
			name, help, name, name, value)
	}
	writeGauge("vlt_collaboration_rooms", "Current collaboration rooms.", int64(rooms.Rooms))
	writeGauge("vlt_collaboration_sockets", "Current collaboration WebSocket connections.", int64(rooms.Sockets))
	writeGauge("vlt_collaboration_queued_bytes", "Bytes currently queued for collaboration sockets.", rooms.QueuedBytes)
	writeGauge("vlt_collaboration_object_cleanup_pending", "Pending durable collaboration staging cleanup jobs.", metrics.objectCleanupPending.Load())
	writeGauge("vlt_collaboration_object_cleanup_running", "Claimed durable collaboration staging cleanup jobs.", metrics.objectCleanupRunning.Load())
	writeGauge("vlt_collaboration_stuck_ending_sessions", "Collaboration sessions waiting too long for a final snapshot.", metrics.stuckEndingSessions.Load())
	writeCounter("vlt_collaboration_operations_total", "Durable collaboration operations committed.", metrics.operations.Load())
	writeCounter("vlt_collaboration_rejections_total", "Collaboration operation submissions rejected.", metrics.rejections.Load())
	writeCounter("vlt_collaboration_resyncs_total", "Collaboration resynchronizations requested.", metrics.resyncs.Load())
	writeCounter("vlt_collaboration_hash_rounds_total", "State hash consensus rounds opened.", metrics.hashRounds.Load())
	writeCounter("vlt_collaboration_hash_timeouts_total", "Writable connections disconnected after hash timeout.", metrics.hashTimeouts.Load())
	writeCounter("vlt_collaboration_upload_verification_success_total", "Upload completion requests verified successfully.", metrics.uploadVerificationSuccess.Load())
	writeCounter("vlt_collaboration_upload_verification_failure_total", "Upload completion requests that failed verification or finalization.", metrics.uploadVerificationFailure.Load())
	writeCounter("vlt_collaboration_snapshot_requests_total", "Snapshot upload requests dispatched.", metrics.snapshotRequests.Load())
	writeCounter("vlt_collaboration_snapshot_retries_total", "Snapshot upload requests dispatched after their first attempt.", metrics.snapshotRetries.Load())
	writeCounter("vlt_collaboration_session_ending_total", "Session ending notifications published.", metrics.sessionEnding.Load())
	writeCounter("vlt_collaboration_session_ended_total", "Sessions closed after a verified final snapshot.", metrics.sessionEnded.Load())
	writeCounter("vlt_collaboration_maintenance_failures_total", "Collaboration maintenance steps that failed.", metrics.maintenanceFailures.Load())
	writeCounter("vlt_collaboration_storage_failures_total", "Collaboration storage operations that failed.", metrics.storageFailures.Load())
	writeCounter("vlt_collaboration_gc_failures_total", "Collaboration blob garbage collection passes that failed.", metrics.gcFailures.Load())
	writeCounter("vlt_collaboration_gc_deletes_total", "Unreferenced collaboration blobs deleted.", metrics.gcDeletes.Load())
	writeCounter("vlt_collaboration_object_cleanup_success_total", "Durable collaboration staging cleanup jobs completed.", metrics.objectCleanupSuccess.Load())
	writeCounter("vlt_collaboration_object_cleanup_failures_total", "Durable collaboration staging cleanup attempts that failed.", metrics.objectCleanupFailures.Load())
	return output.String()
}

func (s *Server) recordCollaborationMaintenanceError(stage string, err error,
	storage, garbageCollection bool) {
	if err == nil {
		return
	}
	log.Printf("collaboration maintenance %s failed: %v", stage, err)
	if s.metrics == nil {
		return
	}
	s.metrics.maintenanceFailures.Add(1)
	if storage {
		s.metrics.storageFailures.Add(1)
	}
	if garbageCollection {
		s.metrics.gcFailures.Add(1)
	}
}

func (s *Server) recordCollaborationStorageError(stage string, err error) {
	if err == nil {
		return
	}
	log.Printf("collaboration storage %s failed: %v", stage, err)
	if s.metrics != nil {
		s.metrics.storageFailures.Add(1)
	}
}
