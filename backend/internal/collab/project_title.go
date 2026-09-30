package collab

import (
	"encoding/json"
	"slices"
)

// There is no field-writer precondition for an unwritten snapshot field. Use
// the whole project revision as its compare-and-swap guard. The normalized
// metadata includes batch children, so a batch cannot bypass this check.
func unguardedProjectTitleWrite(command normalizedOperation) bool {
	if command.SchemaVersion < CollaborationCommandSchemaV6 || !slices.Contains(command.TouchedFields, "project:name") {
		return false
	}
	for _, condition := range command.Preconditions {
		if condition.FieldKey == "project:name" {
			return false
		}
	}
	return true
}

// The last name write inside an atomic batch is also the cloud display title.
func commandProjectTitle(kind string, payload json.RawMessage) (string, bool) {
	if kind == "batch" {
		var batch struct {
			Commands []struct {
				Kind    string          `json:"kind"`
				Payload json.RawMessage `json:"payload"`
			} `json:"commands"`
		}
		if json.Unmarshal(payload, &batch) != nil {
			return "", false
		}
		var title string
		found := false
		for _, command := range batch.Commands {
			if value, ok := commandProjectTitle(command.Kind, command.Payload); ok {
				title, found = value, true
			}
		}
		return title, found
	}
	if kind != "project.setScalar" {
		return "", false
	}
	var scalar struct {
		Field string          `json:"field"`
		Value json.RawMessage `json:"value"`
	}
	if json.Unmarshal(payload, &scalar) != nil || scalar.Field != "name" {
		return "", false
	}
	var title string
	if json.Unmarshal(scalar.Value, &title) != nil {
		return "", false
	}
	normalized, err := validateProjectTitle(title)
	// Older documents permit long/empty musical names; do not turn their replay
	// into a database constraint failure. Valid display titles track the name.
	return normalized, err == nil
}
