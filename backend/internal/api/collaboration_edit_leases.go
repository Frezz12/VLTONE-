package api

import (
	"github.com/google/uuid"
	"net/http"
	"vltstudio/backend/internal/collab"
)

func (s *Server) projectEditLease(w http.ResponseWriter, r *http.Request) {
	projectID, sessionID, ok := collaborationSessionIDs(w, r)
	if !ok {
		return
	}
	var leaseID uuid.UUID
	method := "acquire"
	if r.Method != http.MethodPost {
		leaseID, ok = parseUUIDParam(w, r, "leaseID")
		if !ok {
			return
		}
		method = "renew"
	}
	var input collab.EditLeaseInput
	if r.Method == http.MethodDelete {
		method = "release"
	} else if !decodeJSON(w, r, &input) {
		return
	}
	result, err := s.Collab.ChangeEditLease(r.Context(), projectID, sessionID, userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), leaseID, method, input)
	if err != nil {
		s.writeCollaborationError(w, r, err)
		return
	}
	if method == "release" {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeJSON(w, http.StatusOK, result)
}
