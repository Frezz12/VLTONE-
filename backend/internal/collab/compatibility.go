package collab

import (
	"regexp"
	"strings"

	"vltstudio/backend/internal/model"
)

var semanticVersionPattern = regexp.MustCompile(
	`^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$`)

type ClientCompatibility struct {
	AppVersion           string
	EngineVersion        string
	CommandSchemaVersion int
	ProjectFormatVersion int
}

type semanticVersion struct {
	core       [3]string
	prerelease []string
}

// ValidateClientCompatibility is intentionally fail-closed. Engine/state
// compatibility is exact; application compatibility follows SemVer 2.0.0 and
// rejects an unparseable stored minimum instead of guessing an ordering.
func ValidateClientCompatibility(project model.CloudProject,
	client ClientCompatibility) error {
	if !SupportedCommandSchemaVersion(client.CommandSchemaVersion) ||
		client.ProjectFormatVersion != CollaborationProjectFormatVersion ||
		client.ProjectFormatVersion != project.FormatVersion ||
		client.EngineVersion == "" || client.EngineVersion != project.EngineVersion {
		return incompatiblef("collaboration client is incompatible with the project")
	}
	appVersion, ok := parseSemanticVersion(client.AppVersion)
	if !ok {
		return incompatiblef("application version is not valid semantic versioning")
	}
	minimumVersion, ok := parseSemanticVersion(project.MinimumAppVersion)
	if !ok {
		return incompatiblef("project minimum application version is invalid")
	}
	if compareSemanticVersions(appVersion, minimumVersion) < 0 {
		return incompatiblef("application version is below the project minimum")
	}
	return nil
}

func parseSemanticVersion(value string) (semanticVersion, bool) {
	if value == "" || len(value) > 64 || strings.TrimSpace(value) != value {
		return semanticVersion{}, false
	}
	matches := semanticVersionPattern.FindStringSubmatch(value)
	if matches == nil {
		return semanticVersion{}, false
	}
	result := semanticVersion{core: [3]string{matches[1], matches[2], matches[3]}}
	if matches[4] == "" {
		return result, true
	}
	result.prerelease = strings.Split(matches[4], ".")
	for _, identifier := range result.prerelease {
		if isDecimalIdentifier(identifier) && len(identifier) > 1 && identifier[0] == '0' {
			return semanticVersion{}, false
		}
	}
	return result, true
}

func compareSemanticVersions(left, right semanticVersion) int {
	for index := range left.core {
		if comparison := compareDecimalIdentifiers(left.core[index],
			right.core[index]); comparison != 0 {
			return comparison
		}
	}
	if len(left.prerelease) == 0 && len(right.prerelease) == 0 {
		return 0
	}
	if len(left.prerelease) == 0 {
		return 1
	}
	if len(right.prerelease) == 0 {
		return -1
	}
	limit := min(len(left.prerelease), len(right.prerelease))
	for index := 0; index < limit; index++ {
		leftNumeric := isDecimalIdentifier(left.prerelease[index])
		rightNumeric := isDecimalIdentifier(right.prerelease[index])
		switch {
		case leftNumeric && rightNumeric:
			if comparison := compareDecimalIdentifiers(left.prerelease[index],
				right.prerelease[index]); comparison != 0 {
				return comparison
			}
		case leftNumeric:
			return -1
		case rightNumeric:
			return 1
		case left.prerelease[index] < right.prerelease[index]:
			return -1
		case left.prerelease[index] > right.prerelease[index]:
			return 1
		}
	}
	if len(left.prerelease) < len(right.prerelease) {
		return -1
	}
	if len(left.prerelease) > len(right.prerelease) {
		return 1
	}
	return 0
}

func compareDecimalIdentifiers(left, right string) int {
	if len(left) < len(right) {
		return -1
	}
	if len(left) > len(right) {
		return 1
	}
	if left < right {
		return -1
	}
	if left > right {
		return 1
	}
	return 0
}

func isDecimalIdentifier(value string) bool {
	if value == "" {
		return false
	}
	for index := range len(value) {
		if value[index] < '0' || value[index] > '9' {
			return false
		}
	}
	return true
}
