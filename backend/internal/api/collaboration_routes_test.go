package api

import (
	"net/http"
	"testing"

	"github.com/go-chi/chi/v5"
)

func TestDesktopCollaborationRoutesMatch(t *testing.T) {
	routes, ok := (&Server{}).Router().(chi.Routes)
	if !ok {
		t.Fatal("server router does not expose chi routes")
	}
	id := "11111111-1111-4111-8111-111111111111"
	project := "/v1/desktop/projects/" + id
	tests := []struct {
		method string
		path   string
	}{
		{http.MethodGet, "/v1/desktop/capabilities"},
		{http.MethodPost, "/v1/desktop/project-invites/accept"},
		{http.MethodGet, "/v1/desktop/projects"},
		{http.MethodPost, "/v1/desktop/projects"},
		{http.MethodGet, project},
		{http.MethodPatch, project},
		{http.MethodDelete, project},
		{http.MethodPost, project + "/publish"},
		{http.MethodGet, project + "/bootstrap"},
		{http.MethodGet, project + "/members"},
		{http.MethodPut, project + "/members/" + id},
		{http.MethodDelete, project + "/members/" + id},
		{http.MethodPost, project + "/ownership"},
		{http.MethodGet, project + "/invites"},
		{http.MethodPost, project + "/invites"},
		{http.MethodDelete, project + "/invites/" + id},
		{http.MethodPost, project + "/ops"},
		{http.MethodGet, project + "/ops/" + id},
		{http.MethodPost, project + "/asset-uploads/prepare"},
		{http.MethodPost, project + "/asset-uploads/" + id + "/complete"},
		{http.MethodDelete, project + "/uploads/" + id},
		{http.MethodPost, project + "/snapshot-uploads/prepare"},
		{http.MethodPost, project + "/snapshot-uploads/" + id + "/complete"},
		{http.MethodGet, project + "/assets/" + id + "/download"},
		{http.MethodGet, project + "/snapshots/" + id + "/download"},
		{http.MethodGet, project + "/live"},
		{http.MethodGet, project + "/sessions/active"},
		{http.MethodPost, project + "/sessions"},
		{http.MethodPost, project + "/sessions/" + id + "/join"},
		{http.MethodPost, project + "/sessions/" + id + "/leave"},
		{http.MethodPost, project + "/sessions/" + id + "/heartbeat"},
		{http.MethodPost, project + "/sessions/" + id + "/host"},
		{http.MethodDelete, project + "/sessions/" + id},
		{http.MethodPost, project + "/sessions/" + id + "/leases"},
		{http.MethodPatch, project + "/sessions/" + id + "/leases/" + id},
		{http.MethodDelete, project + "/sessions/" + id + "/leases/" + id},
	}
	for _, test := range tests {
		t.Run(test.method+" "+test.path, func(t *testing.T) {
			context := chi.NewRouteContext()
			if !routes.Match(context, test.method, test.path) {
				t.Fatalf("collaboration route does not match: %s %s",
					test.method, test.path)
			}
		})
	}
}
