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
const schemaKinds = schema.$defs.kind.enum;
const bodyKinds = schema.$defs.nonBatchBody.oneOf.map(
  (shape) => shape.properties.kind.const,
);
bodyKinds.push(
  schema.$defs.bodyShape.oneOf[1].properties.kind.const,
  schema.$defs.bodyShape.oneOf[2].properties.kind.const,
);
assertSameKinds(schemaKinds, bodyKinds, `${schemaPath} bodyShape`);

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
    schema.$defs[schemaDefinition].properties[property].enum,
    cppNames(cppSignature, nextSignature),
    `${label} enum`,
  );
}

// Shareable built-in plugin uids. This set lives in four places and has drifted
// before — a uid accepted by the reducer but absent from the schema or the Go
// validator is a plugin the server silently refuses to relay.
const builtinUidSchema = schema.$defs.sharedInsert.properties.uid.enum;
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
  "protocol: { const: vlt-collab-v3 }",
  "protocols:",
  "project_format: { const: 7 }",
  "command_schema: { const: 3 }",
  "command_schemas:",
  "../../protocol/schema/project-command-v2.schema.json",
  "../../protocol/schema/project-command-v3.schema.json",
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

console.log("Collaboration v2 compatibility and v3 public contracts are pinned.");
