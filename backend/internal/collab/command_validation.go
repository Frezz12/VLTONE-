package collab

import (
	"encoding/json"
	"sort"

	"github.com/google/uuid"
)

const maxBatchCommands = 1024

const recordingClipTrackAssignmentHead = "project:clipTrackAssignments"

type batchCommandPayload struct {
	Kind          string              `json:"kind"`
	Payload       json.RawMessage     `json:"payload"`
	Preconditions []FieldPrecondition `json:"preconditions"`
}

type lifecycleRequirement struct {
	Kind                      lifecycleRequirementKind
	FieldKey                  string
	ExpectedDeleteOperationID *uuid.UUID
}

type lifecycleRequirementKind string

const (
	lifecycleRequireLive    lifecycleRequirementKind = "live"
	lifecycleRequireVacant  lifecycleRequirementKind = "vacant"
	lifecycleRequireRestore lifecycleRequirementKind = "restore"
)

type lifecycleMutation struct {
	FieldKey string
	Effect   lifecycleEffect
}

type lifecycleStep struct {
	Requirements []lifecycleRequirement
	Mutations    []lifecycleMutation
}

type commandLeasePolicy struct {
	BlocksProjectTiming bool
	DeletedTrackIDs     []uuid.UUID
	RoutedTrackIDs      []uuid.UUID
	RecordingLeases     []recordingLeaseReference
}

type recordingLeaseReference struct {
	TrackID uuid.UUID
	LeaseID uuid.UUID
}

type lifecycleEffect string

const (
	lifecycleAlive   lifecycleEffect = "alive"
	lifecycleDeleted lifecycleEffect = "deleted"
)

func deriveCommandMetadata(kind string, payload json.RawMessage, allowBatch bool) ([]string, []FieldPrecondition, error) {
	if err := validateCommandPayloadShape(kind, payload, allowBatch); err != nil {
		return nil, nil, err
	}
	fields := make(map[string]struct{})
	var nestedPreconditions []FieldPrecondition
	add := func(values ...string) {
		for _, value := range values {
			fields[value] = struct{}{}
		}
	}
	addClipDescendants := func(clipID string) {
		add("clip:" + clipID + ":descendants")
	}
	addTrackClipLandingHead := func(trackID string) {
		add("track:" + trackID + ":clipLanding")
	}

	switch kind {
	case "project.setScalar":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		field, err := requiredPayloadString(body, "field")
		if err != nil {
			return nil, nil, err
		}
		switch field {
		case "name", "tempo", "aiInstructions", "renderSampleRate", "masterVolume", "masterPan":
			add("project:" + field)
			if field == "tempo" {
				add("project:tempoCascade")
			}
		default:
			return nil, nil, invalidf("project scalar field is unsupported")
		}
	case "project.setTimeSignature":
		if _, err := commandPayloadObject(payload); err != nil {
			return nil, nil, err
		}
		add("project:timeSignature")
	case "project.setKey":
		if _, err := commandPayloadObject(payload); err != nil {
			return nil, nil, err
		}
		add("project:key")
	case "track.add", "track.restore", "track.delete", "track.move", "track.setProperty", "track.setParent", "track.setOutput":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		trackID, err := requiredPayloadUUID(body, "trackId")
		if err != nil {
			return nil, nil, err
		}
		prefix := "track:" + trackID + ":"
		switch kind {
		case "track.add":
			if err := optionalPayloadUUID(body, "parentId"); err != nil {
				return nil, nil, err
			}
			if err := optionalPayloadUUID(body, "afterId"); err != nil {
				return nil, nil, err
			}
			add(prefix+"lifecycle", prefix+"position")
		case "track.restore":
			if _, err := requiredPayloadUUID(body, "deleteOperationId"); err != nil {
				return nil, nil, err
			}
			add(prefix+"lifecycle", prefix+"position", "project:tempoCascade")
		case "track.delete":
			add(prefix+"lifecycle", "project:tempoCascade")
		case "track.move":
			if err := optionalPayloadUUID(body, "afterId"); err != nil {
				return nil, nil, err
			}
			add(prefix + "position")
		case "track.setProperty":
			property, err := requiredPayloadString(body, "property")
			if err != nil {
				return nil, nil, err
			}
			switch property {
			case "name", "color", "volume", "pan", "muted", "mono", "summing":
				add(prefix + property)
			default:
				return nil, nil, invalidf("track property is unsupported")
			}
		case "track.setParent":
			add(prefix + "parentId")
		case "track.setOutput":
			add(prefix + "outputTrackId")
		}
	case "send.add", "send.restore", "send.delete", "send.move", "send.setProperty":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		var sendID string
		if kind == "send.add" {
			sendID, err = requiredNestedPayloadUUID(body, "send", "id")
		} else {
			sendID, err = requiredPayloadUUID(body, "sendId")
		}
		if err != nil {
			return nil, nil, err
		}
		prefix := "send:" + sendID + ":"
		switch kind {
		case "send.add", "send.restore":
			add(prefix+"lifecycle", prefix+"position")
		case "send.delete":
			add(prefix + "lifecycle")
		case "send.move":
			add(prefix + "position")
		case "send.setProperty":
			property, err := requiredPayloadString(body, "property")
			if err != nil {
				return nil, nil, err
			}
			switch property {
			case "destinationTrackId", "level", "preFader", "enabled":
				add(prefix + property)
			default:
				return nil, nil, invalidf("send property is unsupported")
			}
		}
	case "clip.add", "clip.restore", "clip.delete", "clip.move", "clip.setProperty", "clip.setAsset", "clip.setSampleEdit", "clip.setFade", "clip.setFadeCurve", "clip.setFadeMode", "clip.setPatternOwner", "clip.setMusicalAnalysis":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		trackID, err := requiredPayloadUUID(body, "trackId")
		if err != nil {
			return nil, nil, err
		}
		clipID, err := requiredPayloadUUID(body, "clipId")
		if err != nil {
			return nil, nil, err
		}
		prefix := "clip:" + clipID + ":"
		switch kind {
		case "clip.add":
			if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
				return nil, nil, err
			}
			// AddClip initializes every mutable clip field, even though the v1
			// payload only carries the non-default subset. Keep this list in exact
			// lockstep with C++ commandTouchedFields(AddClip): otherwise an undo
			// accepted by one authority could overwrite a later scalar edit seen by
			// the other.
			add(prefix+"asset",
				prefix+"automationActive",
				prefix+"automationDefaultValue",
				prefix+"automationTarget",
				prefix+"color",
				prefix+"compCrossfadeMs",
				prefix+"durationSeconds",
				prefix+"fadeInSeconds",
				prefix+"fadeOutSeconds",
				prefix+"fadeInCurve",
				prefix+"fadeOutCurve",
				prefix+"fadeInMode",
				prefix+"fadeOutMode",
				prefix+"gain",
				prefix+"lifecycle",
				prefix+"muted",
				prefix+"name",
				prefix+"offsetSeconds",
				prefix+"pan",
				prefix+"patternClipId",
				prefix+"position",
				prefix+"musicalAnalysis",
				prefix+"sampleEdit",
				prefix+"startSeconds")
			addClipDescendants(clipID)
			addTrackClipLandingHead(trackID)
			add("project:tempoCascade")
		case "clip.restore":
			if _, err := requiredPayloadUUID(body, "deleteOperationId"); err != nil {
				return nil, nil, err
			}
			add(prefix+"lifecycle", prefix+"position")
			addClipDescendants(clipID)
			addTrackClipLandingHead(trackID)
			add("project:tempoCascade")
		case "clip.delete":
			add(prefix + "lifecycle")
			addClipDescendants(clipID)
			addTrackClipLandingHead(trackID)
			add("project:tempoCascade")
		case "clip.move":
			if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
				return nil, nil, err
			}
			add(prefix + "position")
			sourceTrackID, err := requiredPayloadUUID(body, "sourceTrackId")
			if err != nil {
				return nil, nil, err
			}
			addTrackClipLandingHead(sourceTrackID)
			addTrackClipLandingHead(trackID)
		case "clip.setProperty":
			property, err := requiredPayloadString(body, "property")
			if err != nil {
				return nil, nil, err
			}
			switch property {
			case "name", "startSeconds", "durationSeconds", "offsetSeconds", "gain", "pan", "muted", "color", "compCrossfadeMs":
				add(prefix + property)
				if property == "startSeconds" || property == "durationSeconds" || property == "offsetSeconds" {
					addTrackClipLandingHead(trackID)
				}
				if property == "startSeconds" || property == "durationSeconds" {
					add("project:tempoCascade")
				}
			default:
				return nil, nil, invalidf("clip property is unsupported")
			}
		case "clip.setAsset":
			add(prefix + "asset")
			addTrackClipLandingHead(trackID)
		case "clip.setSampleEdit":
			add(prefix + "sampleEdit")
			addTrackClipLandingHead(trackID)
		case "clip.setFade":
			add(prefix+"fadeInSeconds", prefix+"fadeOutSeconds")
			addTrackClipLandingHead(trackID)
			add("project:tempoCascade")
		case "clip.setFadeCurve":
			edge, _ := requiredPayloadString(body, "edge")
			if edge == "in" {
				add(prefix + "fadeInCurve")
			} else {
				add(prefix + "fadeOutCurve")
			}
		case "clip.setFadeMode":
			edge, _ := requiredPayloadString(body, "edge")
			if edge == "in" {
				add(prefix + "fadeInMode")
			} else {
				add(prefix + "fadeOutMode")
			}
		case "clip.setPatternOwner":
			add(prefix+"patternClipId", "clip:"+clipID+":descendants")
		case "clip.setMusicalAnalysis":
			add(prefix + "musicalAnalysis")
		}
	case "plugin.add", "plugin.restore", "plugin.delete", "plugin.move", "plugin.replace", "plugin.setProperty", "plugin.setState", "plugin.setParameter", "plugin.removeParameter", "plugin.setAssetBinding", "plugin.removeAssetBinding":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return nil, nil, err
		}
		var insertID string
		if kind == "plugin.add" {
			insertID, err = requiredNestedPayloadUUID(body, "insert", "id")
		} else {
			insertID, err = requiredPayloadUUID(body, "insertId")
		}
		if err != nil {
			return nil, nil, err
		}
		prefix := "plugin:" + insertID + ":"
		switch kind {
		case "plugin.add", "plugin.restore":
			add(prefix+"lifecycle", prefix+"position", prefix+"generation")
		case "plugin.delete":
			add(prefix+"lifecycle", prefix+"generation")
		case "plugin.move":
			add(prefix+"position", prefix+"generation")
		case "plugin.replace":
			add(prefix+"generation", prefix+"state")
		case "plugin.setProperty":
			property, err := requiredPayloadString(body, "property")
			if err != nil {
				return nil, nil, err
			}
			switch property {
			case "name", "bypassed", "mix", "channelMode", "sidechainTrackId":
				add(prefix+property, prefix+"generation")
			default:
				return nil, nil, invalidf("plugin property is unsupported")
			}
		case "plugin.setState":
			add(prefix+"state", prefix+"generation")
		case "plugin.setParameter", "plugin.removeParameter":
			parameterID, err := payloadString(body, "parameterId", maximumPluginParameterIDBytes, false)
			if err != nil {
				return nil, nil, err
			}
			right, err := payloadBool(body, "rightChannel")
			if err != nil {
				return nil, nil, err
			}
			channel := "left:"
			if right {
				channel = "right:"
			}
			add(prefix+"state", prefix+"parameter:"+channel+parameterID, prefix+"generation")
		case "plugin.setAssetBinding":
			binding, err := commandPayloadObject(body["binding"])
			if err != nil {
				return nil, nil, err
			}
			key, err := payloadString(binding, "key", 96, false)
			if err != nil {
				return nil, nil, err
			}
			add(prefix+"state", prefix+"assetBinding:"+key, prefix+"generation")
		case "plugin.removeAssetBinding":
			key, err := payloadString(body, "key", 96, false)
			if err != nil {
				return nil, nil, err
			}
			add(prefix+"state", prefix+"assetBinding:"+key, prefix+"generation")
		}
		if location.ClipID != "" {
			addClipDescendants(location.ClipID)
		}
	case "samplerFx.setLevels":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		instrumentID, err := requiredPayloadUUID(body, "instrumentId")
		if err != nil {
			return nil, nil, err
		}
		add("samplerFx:"+instrumentID+":volume", "samplerFx:"+instrumentID+":pan")
	case "note.upsert", "note.restore", "note.delete":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		if _, err := requiredPayloadUUID(body, "trackId"); err != nil {
			return nil, nil, err
		}
		clipID, err := requiredPayloadUUID(body, "clipId")
		if err != nil {
			return nil, nil, err
		}
		var noteID string
		if kind == "note.upsert" {
			noteID, err = requiredNestedPayloadUUID(body, "note", "id")
			if err != nil {
				return nil, nil, err
			}
			if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
				return nil, nil, err
			}
		} else {
			noteID, err = requiredPayloadUUID(body, "noteId")
			if err != nil {
				return nil, nil, err
			}
		}
		if kind == "note.restore" {
			if _, err := requiredPayloadUUID(body, "deleteOperationId"); err != nil {
				return nil, nil, err
			}
		}
		prefix := "note:" + noteID + ":"
		if kind == "note.delete" {
			add(prefix + "lifecycle")
		} else {
			if kind == "note.restore" {
				add(prefix + "lifecycle")
			}
			add(prefix+"position", prefix+"pitch", prefix+"startBeats", prefix+"lengthBeats",
				prefix+"velocity", prefix+"muted", prefix+"color", prefix+"pan")
		}
		addClipDescendants(clipID)
	case "automationPoint.upsert", "automationPoint.restore", "automationPoint.delete":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		if _, err := requiredPayloadUUID(body, "trackId"); err != nil {
			return nil, nil, err
		}
		clipID, err := requiredPayloadUUID(body, "clipId")
		if err != nil {
			return nil, nil, err
		}
		if err := optionalPayloadUUIDIfPresent(body, "laneId"); err != nil {
			return nil, nil, err
		}
		var pointID string
		if kind == "automationPoint.upsert" {
			pointID, err = requiredNestedPayloadUUID(body, "point", "id")
			if err != nil {
				return nil, nil, err
			}
			if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
				return nil, nil, err
			}
		} else {
			pointID, err = requiredPayloadUUID(body, "pointId")
			if err != nil {
				return nil, nil, err
			}
		}
		if kind == "automationPoint.restore" {
			if _, err := requiredPayloadUUID(body, "deleteOperationId"); err != nil {
				return nil, nil, err
			}
		}
		prefix := "automationPoint:" + pointID + ":"
		if kind == "automationPoint.delete" {
			add(prefix + "lifecycle")
		} else {
			if kind == "automationPoint.restore" {
				add(prefix + "lifecycle")
			}
			add(prefix+"position", prefix+"beats", prefix+"value", prefix+"shape", prefix+"curve")
		}
		addClipDescendants(clipID)
	case "controllerLane.add", "controllerLane.restore", "controllerLane.delete", "controllerLane.setTarget", "controllerLane.setDefault":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		laneID, err := requiredPayloadUUID(body, "laneId")
		if err != nil {
			return nil, nil, err
		}
		clipID, err := requiredPayloadUUID(body, "clipId")
		if err != nil {
			return nil, nil, err
		}
		prefix := "controllerLane:" + laneID + ":"
		switch kind {
		case "controllerLane.add":
			add(prefix+"lifecycle", prefix+"position")
		case "controllerLane.restore":
			add(prefix+"lifecycle", prefix+"position")
		case "controllerLane.delete":
			add(prefix + "lifecycle")
		case "controllerLane.setTarget":
			add(prefix + "target")
		case "controllerLane.setDefault":
			add(prefix + "defaultValue")
		}
		addClipDescendants(clipID)
	case "automation.setTarget", "automation.setDefault", "automation.setActive":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		clipID, err := requiredPayloadUUID(body, "clipId")
		if err != nil {
			return nil, nil, err
		}
		field := "automationTarget"
		if kind == "automation.setDefault" {
			field = "automationDefaultValue"
		} else if kind == "automation.setActive" {
			field = "automationActive"
		}
		add("clip:" + clipID + ":" + field)
	case "take.add", "take.restore", "take.delete", "take.move", "take.setProperty":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		clipID, err := requiredPayloadUUID(body, "clipId")
		if err != nil {
			return nil, nil, err
		}
		var takeID string
		if kind == "take.add" {
			takeID, err = requiredNestedPayloadUUID(body, "take", "id")
		} else {
			takeID, err = requiredPayloadUUID(body, "takeId")
		}
		if err != nil {
			return nil, nil, err
		}
		prefix := "take:" + takeID + ":"
		if kind == "take.delete" {
			add(prefix + "lifecycle")
		} else if kind == "take.move" {
			add(prefix + "position")
		} else if kind == "take.setProperty" {
			property, err := requiredPayloadString(body, "property")
			if err != nil {
				return nil, nil, err
			}
			add(prefix + property)
		} else {
			add(prefix+"lifecycle", prefix+"position", prefix+"name",
				prefix+"offsetSeconds", prefix+"lengthSeconds",
				prefix+"clipOffsetSeconds", prefix+"gain", prefix+"muted",
				prefix+"color")
		}
		addClipDescendants(clipID)
	case "compSegment.upsert", "compSegment.restore", "compSegment.delete":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, nil, err
		}
		clipID, err := requiredPayloadUUID(body, "clipId")
		if err != nil {
			return nil, nil, err
		}
		var segmentID string
		if kind == "compSegment.upsert" {
			segmentID, err = requiredNestedPayloadUUID(body, "segment", "id")
		} else {
			segmentID, err = requiredPayloadUUID(body, "segmentId")
		}
		if err != nil {
			return nil, nil, err
		}
		prefix := "compSegment:" + segmentID + ":"
		if kind == "compSegment.delete" {
			add(prefix + "lifecycle")
		} else {
			if kind == "compSegment.restore" {
				add(prefix + "lifecycle")
			}
			add(prefix+"position", prefix+"takeId", prefix+"startSeconds", prefix+"endSeconds")
		}
		addClipDescendants(clipID)
	case "batch", "recording.commit":
		if !allowBatch {
			return nil, nil, invalidf("nested aggregate commands are unsupported")
		}
		var body struct {
			Commands []batchCommandPayload `json:"commands"`
		}
		if err := json.Unmarshal(payload, &body); err != nil || len(body.Commands) == 0 || len(body.Commands) > maxBatchCommands {
			return nil, nil, invalidf("aggregate command must contain between 1 and %d commands", maxBatchCommands)
		}
		for _, command := range body.Commands {
			childFields, childPreconditions, err := deriveCommandMetadata(command.Kind, command.Payload, false)
			if err != nil {
				return nil, nil, err
			}
			add(childFields...)
			nestedPreconditions = append(nestedPreconditions, command.Preconditions...)
			nestedPreconditions = append(nestedPreconditions, childPreconditions...)
		}
		if kind == "recording.commit" {
			leases, err := recordingCommitLeaseReferences(payload)
			if err != nil {
				return nil, nil, err
			}
			for _, lease := range leases {
				addTrackClipLandingHead(lease.TrackID.String())
			}
		}
	default:
		return nil, nil, invalidf("operation kind is unsupported by command schema version 2")
	}

	result := make([]string, 0, len(fields))
	for field := range fields {
		result = append(result, field)
	}
	sort.Strings(result)
	return result, nestedPreconditions, nil
}

// deriveCommandLeasePolicy extracts the small subset of command metadata that
// must be coordinated with record leases. It walks batches recursively so a
// client cannot bypass the policy by changing the command envelope shape.
func deriveCommandLeasePolicy(kind string, payload json.RawMessage, allowBatch bool) (commandLeasePolicy, error) {
	if err := validateCommandPayloadShape(kind, payload, allowBatch); err != nil {
		return commandLeasePolicy{}, err
	}
	policy := commandLeasePolicy{}
	switch kind {
	case "project.setScalar":
		body, _ := commandPayloadObject(payload)
		field, _ := requiredPayloadString(body, "field")
		// Keep renderSampleRate here as the reserved field for the renderer
		// contract. The v1 payload validator does not expose it yet.
		policy.BlocksProjectTiming = field == "tempo" || field == "renderSampleRate"
	case "project.setTimeSignature":
		policy.BlocksProjectTiming = true
	case "track.delete":
		body, _ := commandPayloadObject(payload)
		trackID, _ := requiredPayloadUUID(body, "trackId")
		parsed, err := uuid.Parse(trackID)
		if err != nil {
			return commandLeasePolicy{}, invalidf("command payload trackId must be a UUID")
		}
		policy.DeletedTrackIDs = []uuid.UUID{parsed}
	case "track.setParent", "track.setOutput", "send.add", "send.delete", "send.restore", "send.move", "send.setProperty":
		body, _ := commandPayloadObject(payload)
		trackID, _ := requiredPayloadUUID(body, "trackId")
		parsed, err := uuid.Parse(trackID)
		if err != nil {
			return commandLeasePolicy{}, invalidf("command payload trackId must be a UUID")
		}
		policy.RoutedTrackIDs = []uuid.UUID{parsed}
	case "plugin.setProperty":
		body, _ := commandPayloadObject(payload)
		property, _ := requiredPayloadString(body, "property")
		if property == "sidechainTrackId" {
			location, err := validatePluginLocation(body["location"])
			if err != nil {
				return commandLeasePolicy{}, err
			}
			if location.TrackID != "" {
				parsed, _ := uuid.Parse(location.TrackID)
				policy.RoutedTrackIDs = []uuid.UUID{parsed}
			}
		}
	case "plugin.replace":
		body, _ := commandPayloadObject(payload)
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return commandLeasePolicy{}, err
		}
		if location.TrackID != "" {
			parsed, _ := uuid.Parse(location.TrackID)
			policy.RoutedTrackIDs = []uuid.UUID{parsed}
		}
	case "batch", "recording.commit":
		var body struct {
			Commands []batchCommandPayload `json:"commands"`
		}
		if err := json.Unmarshal(payload, &body); err != nil {
			return commandLeasePolicy{}, invalidf("aggregate command payload is invalid")
		}
		if kind == "recording.commit" {
			leaseReferences, err := recordingCommitLeaseReferences(payload)
			if err != nil {
				return commandLeasePolicy{}, err
			}
			policy.RecordingLeases = leaseReferences
		}
		for _, command := range body.Commands {
			child, err := deriveCommandLeasePolicy(command.Kind, command.Payload, false)
			if err != nil {
				return commandLeasePolicy{}, err
			}
			policy.BlocksProjectTiming = policy.BlocksProjectTiming || child.BlocksProjectTiming
			policy.DeletedTrackIDs = append(policy.DeletedTrackIDs, child.DeletedTrackIDs...)
			policy.RoutedTrackIDs = append(policy.RoutedTrackIDs, child.RoutedTrackIDs...)
		}
	}
	policy.DeletedTrackIDs = uniqueSortedUUIDs(policy.DeletedTrackIDs)
	policy.RoutedTrackIDs = uniqueSortedUUIDs(policy.RoutedTrackIDs)
	return policy, nil
}

func uniqueSortedUUIDs(values []uuid.UUID) []uuid.UUID {
	seen := make(map[uuid.UUID]struct{}, len(values))
	result := make([]uuid.UUID, 0, len(values))
	for _, value := range values {
		if _, exists := seen[value]; exists {
			continue
		}
		seen[value] = struct{}{}
		result = append(result, value)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].String() < result[j].String() })
	return result
}

func deriveLifecycleSteps(kind string, payload json.RawMessage, allowBatch bool) ([]lifecycleStep, error) {
	if err := validateCommandPayloadShape(kind, payload, allowBatch); err != nil {
		return nil, err
	}
	if kind == "batch" || kind == "recording.commit" {
		var body struct {
			Commands []batchCommandPayload `json:"commands"`
		}
		if err := json.Unmarshal(payload, &body); err != nil {
			return nil, invalidf("batch payload is invalid")
		}
		var steps []lifecycleStep
		for _, command := range body.Commands {
			childSteps, err := deriveLifecycleSteps(command.Kind, command.Payload, false)
			if err != nil {
				return nil, err
			}
			steps = append(steps, childSteps...)
		}
		return steps, nil
	}

	body, err := commandPayloadObject(payload)
	if err != nil {
		return nil, err
	}
	step := lifecycleStep{}
	addRequirement := func(requirementKind lifecycleRequirementKind, fieldKey string, operationID *uuid.UUID) {
		step.Requirements = append(step.Requirements, lifecycleRequirement{
			Kind: requirementKind, FieldKey: fieldKey, ExpectedDeleteOperationID: operationID,
		})
	}
	addLive := func(prefix, identifier string) {
		if identifier != "" {
			addRequirement(lifecycleRequireLive, prefix+identifier+":lifecycle", nil)
		}
	}
	addVacant := func(prefix, identifier string) {
		addRequirement(lifecycleRequireVacant, prefix+identifier+":lifecycle", nil)
	}
	addRestore := func(prefix, identifier string) error {
		deleteOperationID, err := requiredPayloadUUID(body, "deleteOperationId")
		if err != nil {
			return err
		}
		parsed, _ := uuid.Parse(deleteOperationID)
		addRequirement(lifecycleRequireRestore, prefix+identifier+":lifecycle", &parsed)
		return nil
	}
	addMutation := func(prefix, identifier string, effect lifecycleEffect) {
		step.Mutations = append(step.Mutations, lifecycleMutation{
			FieldKey: prefix + identifier + ":lifecycle", Effect: effect,
		})
	}
	identifier := func(name string) (string, error) { return requiredPayloadUUID(body, name) }
	optionalIdentifier := func(name string) (string, error) {
		if _, exists := body[name]; !exists {
			return "", nil
		}
		return optionalPayloadUUIDValue(body, name)
	}
	parents := func(includeLane bool) error {
		trackID, err := identifier("trackId")
		if err != nil {
			return err
		}
		clipID, err := identifier("clipId")
		if err != nil {
			return err
		}
		addLive("track:", trackID)
		addLive("clip:", clipID)
		if includeLane {
			laneID, err := optionalIdentifier("laneId")
			if err != nil {
				return err
			}
			addLive("controllerLane:", laneID)
		}
		return nil
	}
	pluginParents := func() (validatedPluginLocation, error) {
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return validatedPluginLocation{}, err
		}
		addLive("track:", location.TrackID)
		addLive("clip:", location.ClipID)
		return location, nil
	}

	switch kind {
	case "track.add":
		trackID, _ := identifier("trackId")
		parentID, _ := optionalIdentifier("parentId")
		afterID, _ := optionalIdentifier("afterId")
		addVacant("track:", trackID)
		addLive("track:", parentID)
		addLive("track:", afterID)
		addMutation("track:", trackID, lifecycleAlive)
	case "track.delete":
		trackID, _ := identifier("trackId")
		addMutation("track:", trackID, lifecycleDeleted)
	case "track.restore":
		trackID, _ := identifier("trackId")
		if err := addRestore("track:", trackID); err != nil {
			return nil, err
		}
		addMutation("track:", trackID, lifecycleAlive)
	case "track.move":
		trackID, _ := identifier("trackId")
		afterID, _ := optionalIdentifier("afterId")
		addLive("track:", trackID)
		addLive("track:", afterID)
	case "track.setProperty":
		trackID, _ := identifier("trackId")
		addLive("track:", trackID)
	case "track.setParent":
		trackID, _ := identifier("trackId")
		parentID, _ := optionalIdentifier("parentId")
		addLive("track:", trackID)
		addLive("track:", parentID)
	case "track.setOutput":
		trackID, _ := identifier("trackId")
		outputID, _ := optionalIdentifier("outputTrackId")
		addLive("track:", trackID)
		addLive("track:", outputID)
	case "send.add":
		trackID, _ := identifier("trackId")
		send, _ := commandPayloadObject(body["send"])
		sendID, _ := requiredPayloadUUID(send, "id")
		destinationID, _ := requiredPayloadUUID(send, "destinationTrackId")
		afterID, _ := optionalIdentifier("afterId")
		addLive("track:", trackID)
		addLive("track:", destinationID)
		addVacant("send:", sendID)
		addLive("send:", afterID)
		addMutation("send:", sendID, lifecycleAlive)
	case "send.delete":
		trackID, _ := identifier("trackId")
		sendID, _ := identifier("sendId")
		addLive("track:", trackID)
		addMutation("send:", sendID, lifecycleDeleted)
	case "send.restore":
		trackID, _ := identifier("trackId")
		sendID, _ := identifier("sendId")
		addLive("track:", trackID)
		if err := addRestore("send:", sendID); err != nil {
			return nil, err
		}
		addMutation("send:", sendID, lifecycleAlive)
	case "send.move":
		trackID, _ := identifier("trackId")
		sendID, _ := identifier("sendId")
		afterID, _ := optionalIdentifier("afterId")
		addLive("track:", trackID)
		addLive("send:", sendID)
		addLive("send:", afterID)
	case "send.setProperty":
		trackID, _ := identifier("trackId")
		sendID, _ := identifier("sendId")
		addLive("track:", trackID)
		addLive("send:", sendID)
		property, _ := requiredPayloadString(body, "property")
		if property == "destinationTrackId" {
			destinationID, _ := requiredPayloadUUID(body, "value")
			addLive("track:", destinationID)
		}
	case "clip.add":
		if err := parents(false); err != nil {
			return nil, err
		}
		clipID, _ := identifier("clipId")
		afterID, _ := optionalIdentifier("afterId")
		addVacant("clip:", clipID)
		addLive("clip:", afterID)
		addMutation("clip:", clipID, lifecycleAlive)
	case "clip.delete":
		if err := parents(false); err != nil {
			return nil, err
		}
		clipID, _ := identifier("clipId")
		addMutation("clip:", clipID, lifecycleDeleted)
	case "clip.restore":
		if err := parents(false); err != nil {
			return nil, err
		}
		clipID, _ := identifier("clipId")
		if err := addRestore("clip:", clipID); err != nil {
			return nil, err
		}
		addMutation("clip:", clipID, lifecycleAlive)
	case "clip.move":
		if err := parents(false); err != nil {
			return nil, err
		}
		sourceTrackID, _ := identifier("sourceTrackId")
		clipID, _ := identifier("clipId")
		afterID, _ := optionalIdentifier("afterId")
		addLive("track:", sourceTrackID)
		addLive("clip:", clipID)
		addLive("clip:", afterID)
	case "clip.setProperty", "clip.setAsset", "clip.setSampleEdit", "clip.setFade", "clip.setFadeCurve", "clip.setFadeMode", "clip.setMusicalAnalysis", "automation.setDefault", "automation.setActive":
		if err := parents(false); err != nil {
			return nil, err
		}
	case "clip.setPatternOwner":
		if err := parents(false); err != nil {
			return nil, err
		}
		patternID, _ := optionalIdentifier("patternClipId")
		addLive("clip:", patternID)
	case "plugin.add":
		location, err := pluginParents()
		if err != nil {
			return nil, err
		}
		insert, _ := commandPayloadObject(body["insert"])
		insertID, _ := requiredPayloadUUID(insert, "id")
		afterID, _ := optionalIdentifier("afterId")
		addVacant("plugin:", insertID)
		addLive("plugin:", afterID)
		sidechainID, _ := optionalPayloadUUIDValue(insert, "sidechainTrackId")
		addLive("track:", sidechainID)
		if location.Chain == "instrument" && afterID != "" {
			return nil, invalidf("command payload instrument chain cannot have an anchor")
		}
		addMutation("plugin:", insertID, lifecycleAlive)
	case "plugin.delete":
		if _, err := pluginParents(); err != nil {
			return nil, err
		}
		insertID, _ := identifier("insertId")
		addMutation("plugin:", insertID, lifecycleDeleted)
	case "plugin.restore":
		if _, err := pluginParents(); err != nil {
			return nil, err
		}
		insertID, _ := identifier("insertId")
		if err := addRestore("plugin:", insertID); err != nil {
			return nil, err
		}
		addMutation("plugin:", insertID, lifecycleAlive)
	case "plugin.move":
		if _, err := pluginParents(); err != nil {
			return nil, err
		}
		insertID, _ := identifier("insertId")
		afterID, _ := optionalIdentifier("afterId")
		addLive("plugin:", insertID)
		addLive("plugin:", afterID)
	case "plugin.setProperty":
		if _, err := pluginParents(); err != nil {
			return nil, err
		}
		insertID, _ := identifier("insertId")
		addLive("plugin:", insertID)
		property, _ := requiredPayloadString(body, "property")
		if property == "sidechainTrackId" {
			sidechainID, _ := optionalPayloadUUIDValue(body, "value")
			addLive("track:", sidechainID)
		}
	case "plugin.replace":
		if _, err := pluginParents(); err != nil {
			return nil, err
		}
		insertID, _ := identifier("insertId")
		addLive("plugin:", insertID)
		replacement, _ := commandPayloadObject(body["replacement"])
		sidechainID, _ := optionalPayloadUUIDValue(replacement, "sidechainTrackId")
		addLive("track:", sidechainID)
	case "plugin.setState", "plugin.setParameter", "plugin.removeParameter", "plugin.setAssetBinding", "plugin.removeAssetBinding":
		if _, err := pluginParents(); err != nil {
			return nil, err
		}
		insertID, _ := identifier("insertId")
		addLive("plugin:", insertID)
	case "samplerFx.setLevels":
		trackID, _ := identifier("trackId")
		instrumentID, _ := identifier("instrumentId")
		addLive("track:", trackID)
		addLive("plugin:", instrumentID)
	case "note.upsert":
		if err := parents(false); err != nil {
			return nil, err
		}
		noteID, _ := requiredNestedPayloadUUID(body, "note", "id")
		afterID, _ := optionalIdentifier("afterId")
		addLive("note:", noteID)
		addLive("note:", afterID)
	case "note.delete":
		if err := parents(false); err != nil {
			return nil, err
		}
		noteID, _ := identifier("noteId")
		addMutation("note:", noteID, lifecycleDeleted)
	case "note.restore":
		if err := parents(false); err != nil {
			return nil, err
		}
		noteID, _ := identifier("noteId")
		if err := addRestore("note:", noteID); err != nil {
			return nil, err
		}
		addMutation("note:", noteID, lifecycleAlive)
	case "automationPoint.upsert":
		if err := parents(true); err != nil {
			return nil, err
		}
		pointID, _ := requiredNestedPayloadUUID(body, "point", "id")
		afterID, _ := optionalIdentifier("afterId")
		addLive("automationPoint:", pointID)
		addLive("automationPoint:", afterID)
	case "automationPoint.delete":
		if err := parents(true); err != nil {
			return nil, err
		}
		pointID, _ := identifier("pointId")
		addMutation("automationPoint:", pointID, lifecycleDeleted)
	case "automationPoint.restore":
		if err := parents(true); err != nil {
			return nil, err
		}
		pointID, _ := identifier("pointId")
		if err := addRestore("automationPoint:", pointID); err != nil {
			return nil, err
		}
		addMutation("automationPoint:", pointID, lifecycleAlive)
	case "controllerLane.add":
		if err := parents(false); err != nil {
			return nil, err
		}
		laneID, _ := identifier("laneId")
		afterID, _ := optionalIdentifier("afterId")
		addVacant("controllerLane:", laneID)
		addLive("controllerLane:", afterID)
		addMutation("controllerLane:", laneID, lifecycleAlive)
	case "controllerLane.delete":
		if err := parents(false); err != nil {
			return nil, err
		}
		laneID, _ := identifier("laneId")
		addMutation("controllerLane:", laneID, lifecycleDeleted)
	case "controllerLane.restore":
		if err := parents(false); err != nil {
			return nil, err
		}
		laneID, _ := identifier("laneId")
		if err := addRestore("controllerLane:", laneID); err != nil {
			return nil, err
		}
		addMutation("controllerLane:", laneID, lifecycleAlive)
	case "controllerLane.setTarget", "controllerLane.setDefault":
		if err := parents(false); err != nil {
			return nil, err
		}
		laneID, _ := identifier("laneId")
		addLive("controllerLane:", laneID)
	case "automation.setTarget":
		if err := parents(false); err != nil {
			return nil, err
		}
		target, _ := commandPayloadObject(body["target"])
		channelID, _ := requiredPayloadUUID(target, "channelId")
		addLive("track:", channelID)
	case "take.add":
		if err := parents(false); err != nil {
			return nil, err
		}
		takeID, _ := requiredNestedPayloadUUID(body, "take", "id")
		afterID, _ := optionalIdentifier("afterId")
		addVacant("take:", takeID)
		addLive("take:", afterID)
		addMutation("take:", takeID, lifecycleAlive)
	case "take.delete":
		if err := parents(false); err != nil {
			return nil, err
		}
		takeID, _ := identifier("takeId")
		addMutation("take:", takeID, lifecycleDeleted)
	case "take.restore":
		if err := parents(false); err != nil {
			return nil, err
		}
		takeID, _ := identifier("takeId")
		if err := addRestore("take:", takeID); err != nil {
			return nil, err
		}
		addMutation("take:", takeID, lifecycleAlive)
	case "take.move":
		if err := parents(false); err != nil {
			return nil, err
		}
		takeID, _ := identifier("takeId")
		afterID, _ := optionalIdentifier("afterId")
		addLive("take:", takeID)
		addLive("take:", afterID)
	case "take.setProperty":
		if err := parents(false); err != nil {
			return nil, err
		}
		takeID, _ := identifier("takeId")
		addLive("take:", takeID)
	case "compSegment.upsert":
		if err := parents(false); err != nil {
			return nil, err
		}
		segment, _ := commandPayloadObject(body["segment"])
		segmentID, _ := requiredPayloadUUID(segment, "id")
		takeID, _ := requiredPayloadUUID(segment, "takeId")
		afterID, _ := optionalIdentifier("afterId")
		addLive("compSegment:", segmentID)
		addLive("compSegment:", afterID)
		addLive("take:", takeID)
	case "compSegment.delete":
		if err := parents(false); err != nil {
			return nil, err
		}
		segmentID, _ := identifier("segmentId")
		addMutation("compSegment:", segmentID, lifecycleDeleted)
	case "compSegment.restore":
		if err := parents(false); err != nil {
			return nil, err
		}
		segmentID, _ := identifier("segmentId")
		if err := addRestore("compSegment:", segmentID); err != nil {
			return nil, err
		}
		addMutation("compSegment:", segmentID, lifecycleAlive)
	}
	if len(step.Requirements) == 0 && len(step.Mutations) == 0 {
		return nil, nil
	}
	normalized, err := normalizeLifecycleStep(step)
	if err != nil {
		return nil, err
	}
	return []lifecycleStep{normalized}, nil
}

func normalizeLifecycleStep(step lifecycleStep) (lifecycleStep, error) {
	requirements := make(map[string]lifecycleRequirement, len(step.Requirements))
	for _, requirement := range step.Requirements {
		if existing, ok := requirements[requirement.FieldKey]; ok {
			sameOperation := existing.ExpectedDeleteOperationID == nil && requirement.ExpectedDeleteOperationID == nil
			if existing.ExpectedDeleteOperationID != nil && requirement.ExpectedDeleteOperationID != nil {
				sameOperation = *existing.ExpectedDeleteOperationID == *requirement.ExpectedDeleteOperationID
			}
			if existing.Kind != requirement.Kind || !sameOperation {
				return lifecycleStep{}, invalidf("command contains conflicting lifecycle requirements for %q", requirement.FieldKey)
			}
			continue
		}
		requirements[requirement.FieldKey] = requirement
	}
	step.Requirements = step.Requirements[:0]
	for _, requirement := range requirements {
		step.Requirements = append(step.Requirements, requirement)
	}
	sort.Slice(step.Requirements, func(i, j int) bool {
		return step.Requirements[i].FieldKey < step.Requirements[j].FieldKey
	})
	mutations := make(map[string]lifecycleMutation, len(step.Mutations))
	for _, mutation := range step.Mutations {
		if existing, ok := mutations[mutation.FieldKey]; ok && existing.Effect != mutation.Effect {
			return lifecycleStep{}, invalidf("command contains conflicting lifecycle mutations for %q", mutation.FieldKey)
		}
		mutations[mutation.FieldKey] = mutation
	}
	step.Mutations = step.Mutations[:0]
	for _, mutation := range mutations {
		step.Mutations = append(step.Mutations, mutation)
	}
	sort.Slice(step.Mutations, func(i, j int) bool {
		return step.Mutations[i].FieldKey < step.Mutations[j].FieldKey
	})
	return step, nil
}

func lifecycleEffectForOperation(kind string, payload json.RawMessage, fieldKey string) (lifecycleEffect, bool, error) {
	if kind == "batch" || kind == "recording.commit" {
		var body struct {
			Commands []batchCommandPayload `json:"commands"`
		}
		if err := json.Unmarshal(payload, &body); err != nil {
			return "", false, err
		}
		var effect lifecycleEffect
		found := false
		for _, command := range body.Commands {
			childEffect, childFound, err := lifecycleEffectForOperation(command.Kind, command.Payload, fieldKey)
			if err != nil {
				return "", false, err
			}
			if childFound {
				effect, found = childEffect, true
			}
		}
		return effect, found, nil
	}
	mutationField, effect, err := commandLifecycleMutation(kind, payload)
	if err != nil {
		return "", false, err
	}
	return effect, mutationField == fieldKey, nil
}

func commandLifecycleMutation(kind string, payload json.RawMessage) (string, lifecycleEffect, error) {
	identifier, prefix, effect := "", "", lifecycleEffect("")
	switch kind {
	case "track.add", "track.restore":
		identifier, prefix, effect = "trackId", "track:", lifecycleAlive
	case "track.delete":
		identifier, prefix, effect = "trackId", "track:", lifecycleDeleted
	case "clip.add", "clip.restore":
		identifier, prefix, effect = "clipId", "clip:", lifecycleAlive
	case "clip.delete":
		identifier, prefix, effect = "clipId", "clip:", lifecycleDeleted
	case "send.add":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return "", "", err
		}
		entityID, err := requiredNestedPayloadUUID(body, "send", "id")
		if err != nil {
			return "", "", err
		}
		return "send:" + entityID + ":lifecycle", lifecycleAlive, nil
	case "send.restore":
		identifier, prefix, effect = "sendId", "send:", lifecycleAlive
	case "send.delete":
		identifier, prefix, effect = "sendId", "send:", lifecycleDeleted
	case "plugin.add":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return "", "", err
		}
		entityID, err := requiredNestedPayloadUUID(body, "insert", "id")
		if err != nil {
			return "", "", err
		}
		return "plugin:" + entityID + ":lifecycle", lifecycleAlive, nil
	case "plugin.restore":
		identifier, prefix, effect = "insertId", "plugin:", lifecycleAlive
	case "plugin.delete":
		identifier, prefix, effect = "insertId", "plugin:", lifecycleDeleted
	case "note.restore":
		identifier, prefix, effect = "noteId", "note:", lifecycleAlive
	case "note.delete":
		identifier, prefix, effect = "noteId", "note:", lifecycleDeleted
	case "automationPoint.restore":
		identifier, prefix, effect = "pointId", "automationPoint:", lifecycleAlive
	case "automationPoint.delete":
		identifier, prefix, effect = "pointId", "automationPoint:", lifecycleDeleted
	case "controllerLane.add", "controllerLane.restore":
		identifier, prefix, effect = "laneId", "controllerLane:", lifecycleAlive
	case "controllerLane.delete":
		identifier, prefix, effect = "laneId", "controllerLane:", lifecycleDeleted
	case "take.add":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return "", "", err
		}
		entityID, err := requiredNestedPayloadUUID(body, "take", "id")
		if err != nil {
			return "", "", err
		}
		return "take:" + entityID + ":lifecycle", lifecycleAlive, nil
	case "take.restore":
		identifier, prefix, effect = "takeId", "take:", lifecycleAlive
	case "take.delete":
		identifier, prefix, effect = "takeId", "take:", lifecycleDeleted
	case "compSegment.restore":
		identifier, prefix, effect = "segmentId", "compSegment:", lifecycleAlive
	case "compSegment.delete":
		identifier, prefix, effect = "segmentId", "compSegment:", lifecycleDeleted
	default:
		return "", "", nil
	}
	body, err := commandPayloadObject(payload)
	if err != nil {
		return "", "", err
	}
	entityID, err := requiredPayloadUUID(body, identifier)
	if err != nil {
		return "", "", err
	}
	return prefix + entityID + ":lifecycle", effect, nil
}

func commandPayloadObject(payload json.RawMessage) (map[string]json.RawMessage, error) {
	var body map[string]json.RawMessage
	if err := json.Unmarshal(payload, &body); err != nil || body == nil {
		return nil, invalidf("command payload must be an object")
	}
	return body, nil
}

func requiredPayloadString(body map[string]json.RawMessage, name string) (string, error) {
	raw, exists := body[name]
	if !exists {
		return "", invalidf("command payload requires %s", name)
	}
	var value string
	if err := json.Unmarshal(raw, &value); err != nil || value == "" {
		return "", invalidf("command payload %s must be a non-empty string", name)
	}
	return value, nil
}

func requiredPayloadUUID(body map[string]json.RawMessage, name string) (string, error) {
	value, err := requiredPayloadString(body, name)
	if err != nil {
		return "", err
	}
	parsed, err := uuid.Parse(value)
	if err != nil || parsed == uuid.Nil || !isWireUUID(value) {
		return "", invalidf("command payload %s must be a UUID", name)
	}
	return parsed.String(), nil
}

func optionalPayloadUUID(body map[string]json.RawMessage, name string) error {
	raw, exists := body[name]
	if !exists {
		return invalidf("command payload requires %s", name)
	}
	var value string
	if err := json.Unmarshal(raw, &value); err != nil {
		return invalidf("command payload %s must be a UUID or empty string", name)
	}
	if value == "" {
		return nil
	}
	parsed, err := uuid.Parse(value)
	if err != nil || parsed == uuid.Nil || !isWireUUID(value) {
		return invalidf("command payload %s must be a UUID or empty string", name)
	}
	return nil
}

func isWireUUID(value string) bool {
	if len(value) != 36 {
		return false
	}
	for index := range value {
		if index == 8 || index == 13 || index == 18 || index == 23 {
			if value[index] != '-' {
				return false
			}
			continue
		}
		character := value[index]
		if !((character >= '0' && character <= '9') ||
			(character >= 'a' && character <= 'f') ||
			(character >= 'A' && character <= 'F')) {
			return false
		}
	}
	return true
}

func optionalPayloadUUIDIfPresent(body map[string]json.RawMessage, name string) error {
	if _, exists := body[name]; !exists {
		return nil
	}
	return optionalPayloadUUID(body, name)
}

func requiredNestedPayloadUUID(body map[string]json.RawMessage, objectName, name string) (string, error) {
	raw, exists := body[objectName]
	if !exists {
		return "", invalidf("command payload requires %s", objectName)
	}
	nested, err := commandPayloadObject(raw)
	if err != nil {
		return "", err
	}
	return requiredPayloadUUID(nested, name)
}
