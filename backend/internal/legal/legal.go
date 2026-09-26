// Package legal keeps the website's published document revision and operator
// details in the same source as the API's registration validation.
package legal

import (
	"embed"
	"encoding/json"
	"strings"
)

//go:embed profile.json
var files embed.FS

type Profile struct {
	Version                  string `json:"version"`
	OperatorName             string `json:"operator_name"`
	OperatorAddress          string `json:"operator_address"`
	ContactEmail             string `json:"contact_email"`
	DatabaseLocation         string `json:"database_location"`
	RegistrationEnabled      bool   `json:"registration_enabled"`
	RKNNotificationConfirmed bool   `json:"rkn_notification_confirmed"`
	ProcessorsReviewed       bool   `json:"processors_reviewed"`
}

var Current = load()

func load() Profile {
	raw, err := files.ReadFile("profile.json")
	if err != nil {
		panic(err)
	}
	var profile Profile
	if err := json.Unmarshal(raw, &profile); err != nil {
		panic(err)
	}
	return profile
}

func (p Profile) Ready() bool {
	return strings.TrimSpace(p.OperatorName) != "" && strings.TrimSpace(p.OperatorAddress) != "" &&
		strings.TrimSpace(p.ContactEmail) != "" && p.Version != "" && p.DatabaseLocation != "" &&
		p.RKNNotificationConfirmed && p.ProcessorsReviewed
}

// AllowsRegistration is an operational switch. It deliberately does not
// represent completion of the operator's legal and organisational tasks.
func (p Profile) AllowsRegistration() bool {
	return p.RegistrationEnabled && strings.TrimSpace(p.OperatorName) != "" &&
		strings.TrimSpace(p.ContactEmail) != "" && strings.TrimSpace(p.Version) != "" &&
		strings.TrimSpace(p.DatabaseLocation) != ""
}
