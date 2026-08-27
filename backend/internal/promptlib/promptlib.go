// Package promptlib holds the assistant's instructions as this build ships
// them.
//
// The text itself lives in `prompts/` at the root of the repository and is
// turned into builtin_gen.go by scripts/gen_prompts.py — the same source the
// desktop compiles in as its offline fallback. Keeping one source is the whole
// point: an admin who reverts an edited document gets back exactly the text the
// app would have used had it never talked to this server.
package promptlib

// Document is one prompt: the main instructions, or one playbook.
type Document struct {
	ID      string
	Kind    string // "main" or "playbook"
	Title   string
	UseWhen string
	Tags    []string
	Body    string
}

// Find returns the built-in text for one id.
func Find(id string) (Document, bool) {
	for _, doc := range Builtin() {
		if doc.ID == id {
			return doc, true
		}
	}
	return Document{}, false
}
