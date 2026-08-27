package api

import (
	"testing"
	"time"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/promptlib"
)

// The pieces of the prompt library that hold no database. The serving path
// itself is exercised by the Postgres integration test, which is skipped
// without VLT_TEST_DATABASE_URL.

func TestPromptVersionTracksTheText(t *testing.T) {
	now := time.Now().UTC()
	documents := []model.AIPromptDocument{
		{ID: "main", Kind: promptKindMain, Body: "operate the program", Enabled: true, UpdatedAt: now},
		{ID: "bass", Kind: promptKindPlaybook, Body: "follow the root note", Enabled: true, UpdatedAt: now},
	}
	first := promptVersion(documents)
	if first == "" {
		t.Fatal("version is empty")
	}
	// Order must not matter: the rows come back in whatever order the database
	// felt like, and a version that changed with it would defeat the ETag.
	shuffled := []model.AIPromptDocument{documents[1], documents[0]}
	if got := promptVersion(shuffled); got != first {
		t.Fatalf("version depends on row order: %s vs %s", got, first)
	}
	// An edit has to change it, or clients would never download again.
	edited := append([]model.AIPromptDocument(nil), documents...)
	edited[1].Body = "follow the root note, an octave down"
	if got := promptVersion(edited); got == first {
		t.Fatal("an edited body left the version unchanged")
	}
	// A disabled document is not served, so it must not count towards the
	// version either.
	disabled := append([]model.AIPromptDocument(nil), documents...)
	disabled[1].Enabled = false
	if got := promptVersion(disabled); got == first {
		t.Fatal("disabling a document left the version unchanged")
	}
}

func TestPromptIDsMatchWhatTheAppAccepts(t *testing.T) {
	for _, id := range []string{"bass", "vocal-processing", "drum2"} {
		if !validPromptID(id) {
			t.Fatalf("%q should be a valid id", id)
		}
	}
	for _, id := range []string{"", "Bass", "vocal processing", "bass/../etc", "тест"} {
		if validPromptID(id) {
			t.Fatalf("%q should not be a valid id", id)
		}
	}
}

func TestTagsRoundTrip(t *testing.T) {
	if got := tagsOf(tagsJSON([]string{"bass", "808"})); len(got) != 2 || got[0] != "bass" {
		t.Fatalf("tags did not round-trip: %v", got)
	}
	if got := tagsOf(tagsJSON(nil)); len(got) != 0 {
		t.Fatalf("no tags should stay no tags: %v", got)
	}
}

func TestBuiltinLibraryIsUsable(t *testing.T) {
	documents := promptlib.Builtin()
	if len(documents) < 2 {
		t.Fatalf("the built-in library has %d documents", len(documents))
	}
	mains := 0
	for _, doc := range documents {
		if doc.Kind == promptKindMain {
			mains++
		}
		if doc.Body == "" {
			t.Fatalf("%q has no body", doc.ID)
		}
		if !validPromptID(doc.ID) {
			t.Fatalf("%q is not an id the desktop would accept", doc.ID)
		}
	}
	if mains != 1 {
		t.Fatalf("expected exactly one main prompt, found %d", mains)
	}
	// The playbook the bass rule lives in — the reason any of this exists.
	if bass, ok := promptlib.Find("bass"); !ok || bass.UseWhen == "" {
		t.Fatal("the bass playbook is missing or has no use_when line")
	}
	if _, ok := promptlib.Find("no-such-playbook"); ok {
		t.Fatal("Find invented a document")
	}
}
