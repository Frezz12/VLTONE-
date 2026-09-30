import { existsSync, readFileSync } from "node:fs";
import { createRequire } from "node:module";

const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), "utf8");
const requireText = (source, expected, path) => {
  if (!source.includes(expected)) {
    throw new Error(`${path} is missing: ${expected}`);
  }
};

const functionSource = (source, signature) => {
  const start = source.indexOf(signature);
  if (start < 0) throw new Error(`Missing function: ${signature}`);
  const next = source.indexOf("\nfunc ", start + signature.length);
  return source.slice(start, next < 0 ? source.length : next);
};

const quotedValues = (source) =>
  [...source.matchAll(/"([A-Za-z][A-Za-z0-9.]*)"/g)].map((match) => match[1]);

const goSwitchKinds = (source, signature) => {
  const result = [];
  for (const match of functionSource(source, signature).matchAll(/^\tcase\s+((?:"[^"]+"(?:,\s*)?)+):/gm)) {
    result.push(...quotedValues(match[1]));
  }
  return result;
};

const assertSameKinds = (expected, actual, label) => {
  const expectedSet = new Set(expected);
  const actualSet = new Set(actual);
  const missing = [...expectedSet].filter((value) => !actualSet.has(value));
  const extra = [...actualSet].filter((value) => !expectedSet.has(value));
  const duplicates = actual.filter((value, index) => actual.indexOf(value) !== index);
  if (missing.length || extra.length || duplicates.length) {
    throw new Error(
      `${label} command-kind drift; missing=[${missing}], extra=[${extra}], duplicates=[${[...new Set(duplicates)]}]`,
    );
  }
};

if (existsSync(new URL("../docs/vlt-collab-v1.asyncapi.yaml", import.meta.url))) {
  throw new Error("The retired vlt-collab-v1 AsyncAPI contract must not be shipped.");
}

const asyncapiPath = "docs/vlt-collab-v2.asyncapi.yaml";
const asyncapi = read(asyncapiPath);
for (const expected of [
  "id: urn:vltstudio:collaboration:v2",
  "version: 2.0.0",
  "protocol: { const: vlt-collab-v2 }",
  "commandSchemaVersion: { const: 2 }",
  "projectFormatVersion: { const: 7 }",
  "../protocol/schema/project-command-v2.schema.json",
  "name: hash.requested",
  "name: hash.verified",
  "required: [roundId, sessionId, serverSeq, deadlineMs]",
  "required: [roundId, serverSeq, sha256]",
  "writeBlockedReason:",
]) {
  requireText(asyncapi, expected, asyncapiPath);
}

const schemaPath = "protocol/schema/project-command-v2.schema.json";
const schema = JSON.parse(read(schemaPath));
const openapiRequire = createRequire(import.meta.resolve("openapi-typescript"));
const openapiCoreRequire = createRequire(
  openapiRequire.resolve("@redocly/openapi-core"),
);
const Ajv2020 = openapiCoreRequire("@redocly/ajv/dist/2020").default;
const ajv = new Ajv2020({ allErrors: true, strictSchema: true, strictTypes: false });
ajv.addFormat(
  "uuid",
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/,
);
const validateCommand = ajv.compile(schema);
const schemaV3Path = "protocol/schema/project-command-v3.schema.json";
const schemaV3 = JSON.parse(read(schemaV3Path));
const validateCommandV3 = ajv.compile(schemaV3);
const schemas = new Map([[2, schema], [3, schemaV3]]);
const validators = new Map([[2, validateCommand], [3, validateCommandV3]]);
ajv.addSchema(JSON.parse(read("protocol/schema/slide-note.schema.json")));
for (const version of [4, 5, 6]) {
  const next = JSON.parse(read(`protocol/schema/project-command-v${version}.schema.json`));
  schemas.set(version, next);
  validators.set(version, ajv.compile(next));
}
const latest = schemas.get(6);
const definition = (version, name) => {
  const value = schemas.get(version).$defs[name];
  const inherited = value.$ref?.match(/^project-command-v(\d+)\.schema\.json#\/\$defs\/(.+)$/);
  return inherited ? definition(Number(inherited[1]), inherited[2]) : value;
};
const validateFixture = (path) => {
  const value = JSON.parse(read(path));
  const commands = Array.isArray(value.commands) ? value.commands : [value];
  for (const command of commands) {
    if (!validateCommand(command)) {
      throw new Error(`${path} violates ${schemaPath}: ${ajv.errorsText(validateCommand.errors)}`);
    }
  }
};
validateFixture("tests/fixtures/collaboration_take_move_v2.json");
validateFixture("tests/fixtures/collaboration_command_v2_golden.json");
const focusedCommands = JSON.parse(
  read("tests/fixtures/collaboration_command_v2_golden.json"),
).commands;
const mustReject = (command, label) => {
  if (validateCommand(command)) {
    throw new Error(`${schemaPath} accepted invalid ${label}`);
  }
};
const invalidTempo = structuredClone(focusedCommands[0]);
invalidTempo.payload.value = false;
mustReject(invalidTempo, "tempo scalar type");
const optionalSamplerSample = structuredClone(focusedCommands[3]);
optionalSamplerSample.payload.binding.required = false;
mustReject(optionalSamplerSample, "optional Sampler sample binding");
const externalSampler = structuredClone(focusedCommands[2]);
externalSampler.payload.location.chain = "track";
mustReject(externalSampler, "Sampler outside the instrument chain");
const externalPlugin = structuredClone(
  focusedCommands.find((command) => command.kind === "plugin.add"),
);
if (!externalPlugin) throw new Error("v2 golden fixture is missing plugin.add");
externalPlugin.schemaVersion = 3;
Object.assign(externalPlugin.payload.insert, {
  name: "Exact External Effect",
  format: "vst3",
  uid: "com.example.exact-effect",
  vendor: "Example Audio",
  pluginVersion: "1.2.3",
  stateSchemaVersion: 0,
});
if (!validateCommandV3(externalPlugin)) {
  throw new Error(
    `${schemaV3Path} rejected an exact path-free external plugin: ${ajv.errorsText(validateCommandV3.errors)}`,
  );
}
mustReject(externalPlugin, "v3 external plugin in the immutable v2 schema");
const schemaKinds = latest.$defs.kind.enum;
const bodyKinds = latest.$defs.nonBatchBody.oneOf.map(
  (shape) => shape.properties.kind.const,
);
bodyKinds.push(
  definition(6, "bodyShape").oneOf[1].properties.kind.const,
  definition(6, "bodyShape").oneOf[2].properties.kind.const,
);
assertSameKinds(schemaKinds, bodyKinds, "v6 bodyShape");

// Old formats are immutable. Test feature negotiation by changing the version
// on the same payload, so a rejection cannot pass just because of schemaVersion.
const mustValidateVersion = (command, version, expected, label) => {
  const value = { ...structuredClone(command), schemaVersion: version };
  // v6 derives a document-wide render generation on every edit. Preserve the
  // feature payload when negotiating older versions, but adapt this metadata.
  value.touchedFields = value.touchedFields.filter(field => field !== "project:renderGeneration");
  if (version >= 6) value.touchedFields.push("project:renderGeneration");
  const validate = validators.get(version);
  if (validate(value) !== expected) {
    throw new Error(`v${version} ${expected ? "rejected" : "accepted"} ${label}: ${ajv.errorsText(validate.errors)}`);
  }
};
for (const command of focusedCommands) {
  for (const version of [3, 4, 5, 6]) mustValidateVersion(command, version, true, `legacy ${command.kind}`);
}
const versionedFixtures = JSON.parse(read("tests/fixtures/collaboration_command_v4_v6_golden.json")).commands;
for (const command of versionedFixtures.filter(command => command.schemaVersion === 6)) {
  if (!validators.get(6)(command)) throw new Error(`Invalid v6 fixture ${command.kind}`);
  const withoutGeneration = structuredClone(command);
  withoutGeneration.touchedFields = withoutGeneration.touchedFields.filter(field => field !== "project:renderGeneration");
  if (validators.get(6)(withoutGeneration)) throw new Error(`v6 accepted ${command.kind} without its render generation`);
}
for (const command of versionedFixtures) {
  for (const version of [2, 3, 4, 5, 6]) {
    mustValidateVersion(command, version, version >= command.schemaVersion, `${command.kind} introduced in v${command.schemaVersion}`);
  }
}
const fixture = (kind, version) => structuredClone(versionedFixtures.find(command => command.kind === kind && command.schemaVersion === version));
const invalidPart = fixture("recording.prepareMidi", 4);
invalidPart.payload.content.notes[0].channel = 16;
mustValidateVersion(invalidPart, 6, false, "out-of-range MIDI channel");
const localFile = fixture("take.add", 4);
localFile.payload.take.file = "C:/private/recording.mid";
mustValidateVersion(localFile, 6, false, "local MIDI take path");
const invalidSlide = fixture("slide.set", 5);
invalidSlide.payload.slide.points[0].pitch = 128;
mustValidateVersion(invalidSlide, 6, false, "out-of-range slide pitch");
const invalidCue = fixture("project.setNotebookCues", 6);
invalidCue.payload.cues[0].seconds = -1;
mustValidateVersion(invalidCue, 6, false, "negative notebook cue time");
invalidCue.payload.cues[0].seconds = 0;
invalidCue.payload.cues[0].text = "x".repeat(4097);
mustValidateVersion(invalidCue, 6, false, "oversized notebook cue text");
const invalidFingerprint = fixture("plugin.add", 6);
invalidFingerprint.payload.insert.parameterFingerprint = "ABC";
mustValidateVersion(invalidFingerprint, 6, false, "malformed parameter fingerprint");
const largeNotebook = fixture("project.setScalar", 6);
largeNotebook.payload.value = "x".repeat(524288);
mustValidateVersion(largeNotebook, 6, true, "notebook at the ASCII size boundary");
largeNotebook.payload.value += "x";
mustValidateVersion(largeNotebook, 6, false, "oversized notebook");
const batchedCue = { ...fixture("project.setNotebookCues", 6), kind: "batch" };
const cue = fixture("project.setNotebookCues", 6);
batchedCue.payload = { commands: [{ kind: cue.kind, payload: cue.payload, preconditions: [] }] };
mustValidateVersion(batchedCue, 6, true, "notebook inside an atomic batch");
mustValidateVersion(batchedCue, 5, false, "v6 notebook nested in a v5 batch");
const rendered = fixture("clip.setRenderState", 6);
rendered.payload.source.filePath = "C:/private/rendered.wav";
mustValidateVersion(rendered, 6, false, "render result containing a local path");
const freeze = fixture("track.setFreeze", 6);
freeze.payload.asset = null;
mustValidateVersion(freeze, 6, false, "cleared freeze retaining render dimensions");
freeze.payload.durationSeconds = 0;
freeze.payload.sampleRate = 0;
mustValidateVersion(freeze, 6, true, "clearing a freeze");

const commandCppPath = "controller/collaboration/ProjectCommand.cpp";
const commandCpp = read(commandCppPath);
const commandKindSource = commandCpp.slice(
  commandCpp.indexOf("std::string commandKind"),
  commandCpp.indexOf("bool isUuid"),
);
assertSameKinds(
  schemaKinds,
  [...commandKindSource.matchAll(/return\s+"([^"]+)";/g)].map((match) => match[1]),
  `${commandCppPath} commandKind`,
);

const commandHeaderPath = "controller/collaboration/ProjectCommand.hpp";
const commandHeader = read(commandHeaderPath);
const commandBodyMatch = commandHeader.match(
  /using CommandBody = std::variant<([\s\S]*?)>;\s*\n\s*struct ProjectCommand/,
);
if (!commandBodyMatch) throw new Error(`${commandHeaderPath} is missing CommandBody`);
const commandBodyTypes = commandBodyMatch[1]
  .split(",")
  .map((value) => value.trim().replace(/\s+/g, " "));
const mappedBodyTypes = [
  ...commandKindSource.matchAll(/std::is_same_v<T,\s*([^>\s]+)>/g),
].map((match) => match[1]);
mappedBodyTypes.push("std::shared_ptr<BatchCommand>");
assertSameKinds(commandBodyTypes, mappedBodyTypes, `${commandCppPath} typed bodies`);

const cppNames = (signature, nextSignature) => {
  const source = commandCpp.slice(
    commandCpp.indexOf(signature),
    commandCpp.indexOf(nextSignature, commandCpp.indexOf(signature)),
  );
  return [...new Set([...source.matchAll(/return\s+"([^"]+)";/g)].map((match) => match[1]))];
};
for (const [label, cppSignature, nextSignature, schemaDefinition, property] of [
  ["project scalar", "std::string projectScalarName", "bool projectScalarFromName", "setScalarPayload", "field"],
  ["track property", "std::string trackPropertyName", "bool trackPropertyFromName", "trackPropertyPayload", "property"],
  ["clip property", "std::string clipPropertyName", "bool clipPropertyFromName", "clipPropertyPayload", "property"],
  ["take property", "std::string takePropertyName", "bool takePropertyFromName", "takePropertyPayload", "property"],
  ["send property", "std::string sendPropertyName", "bool sendPropertyFromName", "sendPropertyPayload", "property"],
  ["plugin property", "std::string pluginPropertyName", "bool pluginPropertyFromName", "pluginPropertyPayload", "property"],
]) {
  assertSameKinds(
    definition(6, schemaDefinition).properties[property].enum,
    // The typed legacy enum still decodes v2-v5 audition data. V6 excludes
    // durable mute; the separate control action owns it (checked below).
    cppNames(cppSignature, nextSignature).filter((name) => label !== "track property" || name !== "muted"),
    `${label} enum`,
  );
}

const durableMute = structuredClone(focusedCommands[0]);
durableMute.kind = "track.setProperty";
durableMute.payload = {trackId: durableMute.opId, property: "muted", value: true};
durableMute.schemaVersion = 5;
if (!validators.get(5)(durableMute)) throw new Error("Immutable v5 lost durable track mute");
durableMute.schemaVersion = 6;
durableMute.touchedFields.push("project:renderGeneration");
if (validators.get(6)(durableMute)) throw new Error("V6 accepted durable track mute outside session control");
const batchedMute = structuredClone(durableMute);
batchedMute.kind = "batch";
batchedMute.payload = {commands: [{kind: durableMute.kind, payload: durableMute.payload, preconditions: []}]};
if (validators.get(6)(batchedMute)) throw new Error("V6 batch bypassed session audition control");
durableMute.payload.property = "mono";
if (!validators.get(6)(durableMute)) throw new Error("V6 accidentally disabled durable track mono");
const recordingFade = structuredClone(durableMute);
recordingFade.kind = "recording.commit";
recordingFade.payload = {leases: [], commands: [{kind: "clip.setFade", payload: {
  trackId: durableMute.opId, clipId: durableMute.opId, fadeInSeconds: 0.01, fadeOutSeconds: 0.01,
}, preconditions: []}]};
if (!validators.get(6)(recordingFade)) throw new Error(`V6 rejected recording fade: ${ajv.errorsText(validators.get(6).errors)}`);
recordingFade.schemaVersion = 5;
recordingFade.touchedFields = recordingFade.touchedFields.filter((key) => key !== "project:renderGeneration");
if (validators.get(5)(recordingFade)) throw new Error("Immutable v5 accepted a v6 recording fade");

// Shareable built-in plugin uids. This set lives in four places and has drifted
// before — a uid accepted by the reducer but absent from the schema or the Go
// validator is a plugin the server silently refuses to relay.
const builtinUidSchema = definition(6, "builtinSharedInsert").properties.uid.enum;
const reducerPath = "controller/collaboration/ProjectReducer.cpp";
const reducer = read(reducerPath);
const supportedBuiltinSource = reducer.slice(
  reducer.indexOf("bool supportedBuiltin"),
  reducer.indexOf("\n}", reducer.indexOf("bool supportedBuiltin")),
);
if (!supportedBuiltinSource) throw new Error(`${reducerPath} is missing supportedBuiltin`);
assertSameKinds(
  builtinUidSchema,
  [...supportedBuiltinSource.matchAll(/uid == "([^"]+)"/g)].map((match) => match[1]),
  `${reducerPath} supportedBuiltin uid`,
);

const controllerPath = "controller/EngineController.cpp";
const controller = read(controllerPath);
const sharedBuiltinSource = controller.slice(
  controller.indexOf("bool supportedSharedBuiltin"),
  controller.indexOf("\n}", controller.indexOf("bool supportedSharedBuiltin")),
);
if (!sharedBuiltinSource) {
  throw new Error(`${controllerPath} is missing supportedSharedBuiltin`);
}
assertSameKinds(
  builtinUidSchema,
  [...sharedBuiltinSource.matchAll(/uid == "([^"]+)"/g)].map((match) => match[1]),
  `${controllerPath} supportedSharedBuiltin uid`,
);

const preflightPath = "controller/cloud/PublishPreflight.cpp";
const preflight = read(preflightPath);
const preflightUidMatch = preflight.match(/kBuiltinUids\s*\{([\s\S]*?)\}/);
if (!preflightUidMatch) throw new Error(`${preflightPath} is missing kBuiltinUids`);
assertSameKinds(
  builtinUidSchema,
  [...preflightUidMatch[1].matchAll(/"([^"]+)"/g)].map((match) => match[1]),
  `${preflightPath} kBuiltinUids`,
);

const commandJsonPath = "controller/collaboration/CommandJson.cpp";
const commandJson = read(commandJsonPath);
const parseBodySource = commandJson.slice(
  commandJson.indexOf("bool parseBody"),
  commandJson.indexOf("\n} // namespace", commandJson.indexOf("bool parseBody")),
);
assertSameKinds(
  schemaKinds,
  [...parseBodySource.matchAll(/^\s*if \(kind == "([^"]+)"\)/gm)].map(
    (match) => match[1],
  ),
  `${commandJsonPath} parseBody`,
);

const payloadValidationPath = "backend/internal/collab/payload_validation.go";
const payloadValidation = read(payloadValidationPath);
assertSameKinds(
  schemaKinds,
  goSwitchKinds(payloadValidation, "func validateCommandPayloadShapeForSchema"),
  `${payloadValidationPath} validateCommandPayloadShape`,
);

const goSharedInsert = functionSource(payloadValidation, "func validateSharedInsert");
const goBuiltins = [...goSharedInsert.matchAll(/uid != "([^"]+)"/g)].map(
  (match) => match[1],
);
if (!goBuiltins.length)
  throw new Error(`${payloadValidationPath} is missing the shared-insert uid enum`);
assertSameKinds(
  builtinUidSchema,
  goBuiltins,
  `${payloadValidationPath} validateSharedInsert uid`,
);

const metadataPath = "backend/internal/collab/command_validation.go";
const metadata = read(metadataPath);
assertSameKinds(
  schemaKinds,
  goSwitchKinds(metadata, "func deriveCommandMetadataForSchema"),
  `${metadataPath} deriveCommandMetadata`,
);

const assetValidationPath = "backend/internal/collab/asset_command_validation.go";
const assetValidation = read(assetValidationPath);
assertSameKinds(
  [
    "take.add",
    "clip.setAsset",
    "plugin.add",
    "plugin.setState",
    "plugin.replace",
    "plugin.setAssetBinding",
    "track.setFreeze",
    "clip.setRenderState",
    "batch",
    "recording.commit",
  ],
  goSwitchKinds(assetValidation, "func commandAssetRequirements"),
  `${assetValidationPath} asset-bearing commands`,
);

const openapiPath = "backend/openapi/openapi.yaml";
const openapi = read(openapiPath);
for (const expected of [
  "version: 1.0.0",
  "/v1/desktop/capabilities:",
  "operationId: listCloudProjectInvites",
  "protocol: { const: vlt-collab-v6 }",
  "protocols:",
  "project_format: { const: 7 }",
  "command_schema: { const: 6 }",
  "command_schemas:",
  "../../protocol/schema/project-command-v2.schema.json",
  "../../protocol/schema/project-command-v3.schema.json",
  "../../protocol/schema/project-command-v4.schema.json",
  "../../protocol/schema/project-command-v5.schema.json",
  "../../protocol/schema/project-command-v6.schema.json",
  "operationId: updateCloudProjectSessionReadiness",
  "operationId: activateCloudProjectSession",
  "recording: { const: true }",
  "collaboration_not_enabled",
  "hash_consensus_required",
  "cloud_recording_disabled",
  "storage_quota_exceeded",
  "upload_concurrency_exceeded",
]) {
  requireText(openapi, expected, openapiPath);
}

const asyncapiV3Path = "docs/vlt-collab-v3.asyncapi.yaml";
const asyncapiV3 = read(asyncapiV3Path);
for (const expected of [
  "id: urn:vltstudio:collaboration:v3",
  "version: 3.0.0",
  "Sec-WebSocket-Protocol: { const: vlt-collab-v3 }",
  "commandSchemaVersion: { const: 3 }",
  "projectFormatVersion: { const: 7 }",
  "../protocol/schema/project-command-v3.schema.json",
  "name: session.readiness_changed",
  "name: session.activated",
  "session_starting",
  "plugin_not_ready",
]) {
  requireText(asyncapiV3, expected, asyncapiV3Path);
}

const asyncapiV6Path = "docs/vlt-collab-v6.asyncapi.yaml";
const asyncapiV6 = read(asyncapiV6Path);
const yaml = openapiCoreRequire("js-yaml");
const parsedV6 = yaml.load(asyncapiV6);
if (parsedV6.info.version !== "6.0.0" ||
    parsedV6.components.schemas.HelloPayload.properties.commandSchemaVersion.const !== 6 ||
    parsedV6.components.schemas.HelloPayload.properties.projectFormatVersion.const !== 7) {
  throw new Error("v6 AsyncAPI negotiated versions drifted");
}
for (const expected of [
  "id: urn:vltstudio:collaboration:v6",
  "../protocol/schema/project-command-v6.schema.json",
  "../protocol/schema/session-control-v6.schema.json",
  "name: session.control_changed", "name: session.requirements_changed",
  "name: session.mode_changed", "name: session.participant_moderated",
  "name: clock.pong",
]) requireText(asyncapiV6, expected, asyncapiV6Path);
const validateAction = ajv.compile(JSON.parse(read("protocol/schema/session-control-action-v6.schema.json")));
const action = { actionId: "00000000-0000-4000-8000-000000000001", expectedSessionVersion: 1, kind: "seek", positionSeconds: 12.5 };
if (!validateAction(action)) throw new Error("Session action schema rejected a valid seek");
delete action.positionSeconds;
if (validateAction(action)) throw new Error("Session action schema accepted a seek without position");
Object.assign(action, {kind: "audition", trackId: action.actionId, muted: true});
if (!validateAction(action)) throw new Error("Session action schema rejected a valid mute");
action.positionSeconds = 2;
if (validateAction(action)) throw new Error("Session action schema accepted audition with transport fields");
const validateControl = ajv.compile(JSON.parse(read("protocol/schema/session-control-v6.schema.json")));
const control = {
  mode: "synchronized", sessionVersion: 1, hostMemberId: null,
  transport: { revision: 0, playing: false, positionSeconds: 0, rate: 1,
    serverTimeMs: 0, effectiveAtServerMs: 0, loopEnabled: false,
    loopStartSeconds: 0, loopEndSeconds: 0 },
  audition: { revision: 0, mutedTrackIds: [], soloTrackIds: [] },
};
if (!validateControl(control)) throw new Error(`Invalid control fixture: ${ajv.errorsText(validateControl.errors)}`);
control.transport.rate = 0;
if (validateControl(control)) throw new Error("Session control schema accepted zero playback rate");
control.transport.rate = 1;
delete control.transport.playing;
if (validateControl(control)) throw new Error("Session control schema defaulted missing playback state");

// Optional files emitted by the native codec tests verify actual serialized
// commands against these schemas, without checking generated files into Git.
for (const fixturePath of process.argv.slice(2)) {
  const command = JSON.parse(readFileSync(fixturePath, "utf8"));
  const validate = validators.get(command.schemaVersion);
  if (!validate || !validate(command))
    throw new Error(`Native codec fixture ${fixturePath} violates v${command.schemaVersion}: ${validate ? ajv.errorsText(validate.errors) : "unsupported version"}`);
}

console.log("Collaboration v2-v6 schemas, feature negotiation and current client/server command parity are pinned.");
