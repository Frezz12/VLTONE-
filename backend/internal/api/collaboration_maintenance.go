package api

import (
	"context"
	"encoding/json"
	"time"

	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
)

// RunCollaborationMaintenance owns the v1 single-instance lifecycle ticker.
// The database recheck is authoritative, so a reconnect racing the timer keeps
// the room alive. The caller controls shutdown through ctx.
func (s *Server) RunCollaborationMaintenance(ctx context.Context) {
	if !s.Config.CollaborationEnabled || s.Collab == nil {
		return
	}
	grace := time.Duration(s.Config.CollabReconnectSeconds) * time.Second
	if grace < 5*time.Second {
		grace = 30 * time.Second
	}
	staleAfter := time.Duration(s.Config.CollabMemberStaleSeconds) * time.Second
	if staleAfter < collab.MinimumMemberStaleAfter ||
		staleAfter > collab.MaximumMemberStaleAfter {
		staleAfter = 45 * time.Second
	}
	reaperBatch := s.Config.CollabReaperBatch
	if reaperBatch < 1 || reaperBatch > collab.MaximumReaperBatch {
		reaperBatch = 256
	}
	storageBatch := s.Config.CollabStorageReaperBatch
	if storageBatch < 1 || storageBatch > collab.MaximumStorageBatch {
		storageBatch = 128
	}
	maintenanceTimeout := time.Duration(s.Config.CollabMaintenanceTimeoutSeconds) * time.Second
	if maintenanceTimeout < 5*time.Second || maintenanceTimeout > 2*time.Minute {
		maintenanceTimeout = 20 * time.Second
	}
	snapshotAge := time.Duration(s.Config.CollabSnapshotSeconds) * time.Second
	snapshotRetry := time.Duration(s.Config.CollabSnapshotRetrySeconds) * time.Second
	snapshotOps := s.Config.CollabSnapshotOps
	blobRetention := time.Duration(s.Config.CollabBlobRetentionSeconds) * time.Second
	interval := grace / 2
	if staleInterval := staleAfter / 3; staleInterval < interval {
		interval = staleInterval
	}
	if interval < 5*time.Second {
		interval = 5 * time.Second
	}
	if interval > 30*time.Second {
		interval = 30 * time.Second
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			passCtx, cancel := context.WithTimeout(ctx, maintenanceTimeout)
			s.runCollaborationMaintenancePass(passCtx, grace, staleAfter,
				reaperBatch, storageBatch, snapshotOps, snapshotAge,
				snapshotRetry, blobRetention)
			cancel()
		}
	}
}

func (s *Server) runCollaborationMaintenancePass(ctx context.Context,
	grace, staleAfter time.Duration, reaperBatch, storageBatch int,
	snapshotOps int64, snapshotAge, snapshotRetry, blobRetention time.Duration) {
	reaped, err := s.Collab.ReapStaleSessionMembers(ctx, staleAfter, reaperBatch)
	if err == nil {
		s.publishReapedSessionMembers(reaped)
	}

	transitions, err := s.Collab.EndIdleSessions(ctx, grace)
	if err == nil {
		for _, session := range transitions {
			if session.Finalized {
				s.publishSnapshotFinalization(&collab.SnapshotFinalization{
					ProjectID: session.ProjectID, SessionID: session.SessionID,
					FinalSeq: session.FinalSeq,
				})
			} else {
				s.publishSessionEnding(session.ProjectID, session.SessionID,
					session.FinalSeq)
			}
		}
	}

	dispatches, err := s.Collab.ScheduleSnapshotRequests(ctx, snapshotOps,
		snapshotAge, snapshotRetry, reaperBatch)
	if err == nil {
		for _, dispatch := range dispatches {
			s.publishSnapshotDispatch(dispatch)
		}
	}

	if s.CollabAssets == nil {
		return
	}
	_, _ = s.CollabAssets.ReapExpiredUploads(ctx, storageBatch)
	_, _ = s.CollabAssets.RefreshUnreferencedBlobs(ctx, storageBatch)
	_, _ = s.CollabAssets.GarbageCollectUnreferencedBlobs(ctx,
		blobRetention, storageBatch)
}

func (s *Server) publishSnapshotDispatch(dispatch collab.SnapshotDispatch) {
	if s.Rooms == nil || dispatch.HostMemberID == uuid.Nil {
		return
	}
	payload, _ := json.Marshal(dispatch)
	s.Rooms.DeliverParticipant(dispatch.ProjectID, dispatch.HostMemberID,
		collab.RoomMessage{Data: collaborationEnvelope(
			"snapshot.requested", payload, uuid.Nil, nil, 0)})
}

func (s *Server) publishSessionEnding(projectID, sessionID uuid.UUID,
	finalSeq int64) {
	if s.Rooms == nil {
		return
	}
	payload, _ := json.Marshal(map[string]any{
		"sessionId": sessionID, "finalServerSeq": finalSeq,
		"snapshotRequired": true, "graceDeadlineMs": time.Now().UnixMilli(),
	})
	s.Rooms.Publish(projectID, uuid.Nil, collab.RoomMessage{Data: collaborationEnvelope(
		"session.ending", payload, uuid.Nil, nil, 0)})
}

func (s *Server) publishSnapshotFinalization(finalization *collab.SnapshotFinalization) {
	if finalization == nil {
		return
	}
	if s.Rooms != nil {
		payload, _ := json.Marshal(map[string]any{
			"sessionId":        finalization.SessionID,
			"finalServerSeq":   finalization.FinalSeq,
			"snapshotRequired": false,
		})
		s.Rooms.PublishFinal(finalization.ProjectID,
			collab.RoomMessage{Data: collaborationEnvelope(
				"session.ended", payload, uuid.Nil, nil, 0)}, collab.RoomClose{
				Code: "session_ended", Reason: "live session ended",
			})
	}
	if s.Hashes != nil {
		s.Hashes.ClearProject(finalization.ProjectID)
	}
}

func (s *Server) publishReapedSessionMembers(events []collab.ReapedSessionMembers) {
	if s.Rooms == nil {
		return
	}
	for _, event := range events {
		for _, memberID := range event.MemberIDs {
			payload, _ := json.Marshal(map[string]any{
				"participantId": memberID, "reason": "heartbeat_timeout",
			})
			s.Rooms.Publish(event.ProjectID, memberID, collab.RoomMessage{
				Data: collaborationEnvelope("presence.leave", payload,
					uuid.Nil, nil, 0),
			})
			s.Rooms.DisconnectParticipant(memberID, collab.RoomClose{
				Code: "heartbeat_timeout", Reason: "participant heartbeat timed out",
			})
		}
		if !sameOptionalUUID(event.PreviousHostMemberID, event.HostMemberID) {
			payload, _ := json.Marshal(map[string]any{
				"hostParticipantId": event.HostMemberID,
				"reason":            "heartbeat_timeout",
			})
			s.Rooms.Publish(event.ProjectID, uuid.Nil, collab.RoomMessage{
				Data: collaborationEnvelope("session.host_changed", payload,
					uuid.Nil, nil, 0),
			})
		}
	}
}

func (s *Server) ShutdownCollaboration() {
	if s.Rooms != nil {
		s.Rooms.Shutdown(collab.RoomClose{
			Code: "server_shutdown", Reason: "collaboration server restarting",
		})
	}
}

func (s *Server) disconnectCollaborationDevice(deviceID uuid.UUID, code string) {
	if s.Rooms == nil || deviceID == uuid.Nil {
		return
	}
	s.Rooms.DisconnectDevice(deviceID, collab.RoomClose{
		Code: code, Reason: "device collaboration access revoked",
	})
}

func (s *Server) disconnectCollaborationDesktopSession(desktopSessionID uuid.UUID,
	code string) {
	if s.Rooms == nil || desktopSessionID == uuid.Nil {
		return
	}
	s.Rooms.DisconnectDesktopSession(desktopSessionID, collab.RoomClose{
		Code: code, Reason: "desktop collaboration session revoked",
	})
}

func (s *Server) disconnectCollaborationUser(userID uuid.UUID, code string) {
	if s.Rooms == nil || userID == uuid.Nil {
		return
	}
	s.Rooms.DisconnectUser(userID, collab.RoomClose{
		Code: code, Reason: "account collaboration access revoked",
	})
}
