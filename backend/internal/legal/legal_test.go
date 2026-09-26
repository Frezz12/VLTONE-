package legal

import "testing"

func TestPublicationReadinessDoesNotInventMissingOperatorDetails(t *testing.T) {
	profile := Profile{Version: "test", OperatorName: "Test operator", ContactEmail: "test@example.test", DatabaseLocation: "Moscow"}
	if profile.Ready() {
		t.Fatal("incomplete profile ready")
	}
	profile.OperatorAddress = "Test postal address"
	profile.RKNNotificationConfirmed = true
	if profile.Ready() {
		t.Fatal("unreviewed processors ready")
	}
	profile.ProcessorsReviewed = true
	if !profile.Ready() {
		t.Fatal("completed profile not ready")
	}
}

func TestRegistrationSwitchDoesNotClaimLegalReadiness(t *testing.T) {
	profile := Current
	if profile.Ready() {
		t.Fatal("unconfirmed legal profile marked complete")
	}
	if !profile.AllowsRegistration() {
		t.Fatal("registration switch should permit account creation")
	}
	profile.RegistrationEnabled = false
	if profile.AllowsRegistration() {
		t.Fatal("disabled registration switch ignored")
	}
	profile.RegistrationEnabled = true
	profile.ContactEmail = ""
	if profile.AllowsRegistration() {
		t.Fatal("registration allowed without an operator contact")
	}
}
