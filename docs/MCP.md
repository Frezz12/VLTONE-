# External agents / MCP

VLTONE includes a native MCP server and the `vlt_mcp` stdio bridge. The bridge
connects to the running app through a local socket restricted to the current OS
account. Neither Python nor Node.js is needed to connect. The server starts
disabled; its enabled state is remembered between app launches.

## Connect a client

1. Start VLTONE and open **Settings → External agents / MCP**.
2. Enable the server and check that its status says **Listening**.
3. Use **Copy configuration for Codex** for TOML, or **Copy configuration for
   other clients** for JSON. Paste the configuration into the client's MCP
   settings and reconnect that client.

The generated configuration points to the bundled bridge and includes
`command`, `args`, and `env`. Its private `VLTONE_MCP_TOKEN` is passed through the
bridge's environment, never as a command-line argument or in the activity log.
Keep this configuration private. **Revoke access** disconnects existing clients,
disables the server, and replaces the token; copy a fresh configuration to
reconnect. **Cancel active tasks** cancels outstanding jobs without discarding
completed edits. Closing VLTONE ends the connection.

Example shape (use the real values copied from Settings):

```toml
[mcp_servers.vltone]
command = 'C:\Program Files\VLTONE\bin\vlt_mcp.exe'
args = []

[mcp_servers.vltone.env]
VLTONE_MCP_TOKEN = "<copied token>"
```

On macOS the bridge is inside `VLTONE.app/Contents/MacOS`. Only one VLTONE
instance per OS account owns the default endpoint. A second app instance reports
that the endpoint is already in use.

The transport supports MCP `2026-07-28` stateless requests with per-request
version/capability metadata and `server/discover`, and `2025-11-25` initialization
and notifications. `2025-06-18` is also accepted for existing clients. The
protocol implementation is separate from the DAW operations.

## Target a project and document explicitly

Begin with `workspace_projects` and `action: "list"`. Each entry contains
`projectId`, `generation`, `projectRevision`, selection, audio, dirty, and
read-only state. Read-only project tools need `projectId`. Mutations additionally
need the current `generation` and `projectRevision`. Use returned stamps, or
read `get_project` again before writing.

```json
{
  "name": "set_tempo",
  "arguments": {
    "projectId": "<listed projectId>",
    "generation": 1,
    "projectRevision": 7,
    "bpm": 112
  }
}
```

Creator and Lua use separate `documentId` and `revision` values. Discover them
with `creator_document` / `lua_document`, `action: "list"`, or create/open an
editor document. Read the latest `creator_context` / `lua_context` before an
edit. The documents are the same windows the user edits. New/open creates a
separate editor rather than replacing another document. Manually replacing a
document changes its identity. Switching the selected project never redirects
an already targeted operation.

A stale identity/revision returns a structured `conflict`; an occupied editor
returns `busy`. Read the current state and decide whether to retry. Locked tracks
and clips retain their existing restrictions. Collaborative projects retain the
Script SDK's read-only restriction for external automation.

Each synchronous edit or atomic `project_edit` batch is one Undo step. Undo is
project history shared with the user; it can undo their last edit too. A failed
atomic batch restores the project and its history. A connection never combines
all of its edits into one history entry.

## Tools and resources

Existing AI tool names are preserved. Use `tools/list` for current argument
schemas, and `resources/list` / `resources/read` for SDK files, API contracts,
examples and AI playbooks.

| Tools | Operations |
| --- | --- |
| Existing music AI tools | Tracks, notes, clips, instruments, effects, routing, automation, mix controls, transport, import, export and history |
| `workspace_projects` | List, create, open, select, save, close and activate a project's audio engine |
| `project_edit` | Atomic Script SDK `project.apply` operations, including detailed clip/device edits |
| `creator_document`, existing `creator_*` tools | Shared graph/C++ documents, SDK, source editing, ports, compilation, diagnostics, installation and instance updates |
| `lua_document`, existing `lua_*` tools | Shared source/manifest documents, syntax checks, preview on a project copy, installation, live execution and stop |
| `plugin_library` | Catalog, search folders, scanning/results, local `.vltmini` installation, adding modules and channel-strip presets |
| `plugin_parameters` | Paginated host parameter metadata, values, and writes guarded by device identity |
| `recording` | Start on explicitly named tracks and directory, status, and stop |
| `render_preview`, `analyze_sample`, `analyze_track`, `analyze_mix` | Snapshot renders and actual audio measurements |
| `capture_view`, `dsp_status` | Real window PNGs (including a client's Lua extension windows) and device DSP status |
| `script_ui` | Inspect named widgets in your Lua run, click enabled native buttons/toggles, select tabs, and exercise canvas pointer/keyboard callbacks |
| `workspace_ui`, existing command/focus tools | Selection, editors, and supported application commands |
| `get_job`, `cancel_job` | Connection-owned job status, result, diagnostics and cancellation |

Start with `vltone://guide`, `vltone://lua/api`, and the appropriate Creator SDK
and playbook resources. Music AI tools use bars where specified; Script SDK
edits use beats. `plugin_parameters/list` returns the `identity` to pass with
`parameter` for `get` / `set`. Host parameter metadata determines writable
ranges and read-only parameters.

Project save paths are absolute **project directories**. Existing packages are
staged with their additional files and replaced only after a successful write
and revision check. Another open project cannot be overwritten. Opening the
same path returns its existing identity. Closing dirty work requires an explicit
`discard: true` or a successful save first.

Open accepts a project directory or its active `.vlt` manifest (for example,
`Neon Dusk/Neon Dusk.vlt`). Both forms resolve to the same project identity.
File operations take explicit paths and never open a file picker. Audio import,
sampler loading and sample analysis accept an absolute `path` as an alternative
to catalog `contentId`. Render outputs must be new `.wav` paths; omitting `path`
creates a private artifact under VLTONE's cache. Optional render ranges need both
`startSeconds` and `endSeconds`. Activate the target project's engine using
`workspace_projects/audio` before operations requiring its audio runtime.

Commands that would open a modal dialog are reported unavailable. Use the
parameterized project, editor, plugin or Script SDK operation instead.

## Jobs, verification and concurrent work

Compilation, Lua validation, scanning, opening/saving, audio analysis and renders
return `jobId`. Poll `get_job` until `state` is `completed`, `failed`, or
`cancelled`. Inspect `result` and `error`; accepting a job is not a successful
build or render. IDs already identify a pinned project/document and belong only
to the connection that created them. Progress is provided where the underlying
worker reports it; some compiler and script tasks have indeterminate progress.

`notifications/cancelled` can cancel jobs produced by that request; `cancel_job`
cancels by job ID. Disconnection, revocation or project closure cancels unfinished
owned work. Published edits remain. Incomplete renders/saves are not published.
A stale Creator build or Lua installation cannot replace newer source/module
state. User edits can continue during snapshot renders; the resulting audio is
tagged with `snapshotRevision`.

Render results contain a real WAV path, frame count, duration, peak, RMS,
clipping and frequency-band measurements. `capture_view` returns a PNG path and
MCP image content for the visible main, Creator, Lua editor or running script window.
Native GPU surfaces are captured through VLTONE's scene readback and composited
with window chrome; `rendering` reports `gpu` or `widgets`. An unexposed GPU
window returns an error instead of publishing an incomplete backing-store image.
For a script window, use `view: "script"`, the run ID from `get_job.result.id`,
and a `windowId` from `script_ui/list`. `script_ui/get` reads actual widget state;
`click` queues the real button callback and `tab` selects a one-based tab index.
For custom canvases, `pointer` sends a click at normalized `x`/`y` coordinates
(0–1); `key` supports Left/Right/Up/Down/Space/Return/R. These events go only to
the named canvas in the caller's script, never to arbitrary application windows.
Writes require current project stamps and the selected project. Hidden/disabled
controls, completed runs and another client's runs are rejected. Inspect the run
and widget again after a click; acceptance alone does not mean its callback succeeded.
Verify state and
artifacts after changes; do not infer a successful check from a submitted call.

Lua launched by MCP receives ephemeral project/file grants without permission
dialogs. For files outside the project/package, pass explicit `fileAccess`
entries (`path`, `write`, `folder`) to `lua_preview`, `lua_document/run`, or
`run_script`. Preview still writes only to the project copy and uses the normal
preview restrictions. Network and process execution remain denied, and these
grants are not persisted to the installed script's settings. Installed packages
can also be run through the existing `run_script` tool.

The service has bounded input and job queues (8 MiB per message, 16 connections,
32 active jobs, 128 retained job records). GUI/model operations run on the Qt
thread, snapshot workers run in the background, and no MCP code runs in the
audio callback. The local service does not expose an internet listener or
download/run third-party system installers.

## Development checks

Build `daw`, `vlt_mcp`, and `mcp_protocol_test`, then run:

```powershell
ctest --test-dir build-project-tabs --output-on-failure -R '^mcp_'
```

`mcp_protocol` exercises both protocol generations, discovery, errors,
cancellation and schema validation. `mcp_integration` starts a real native
bridge against an isolated VLTONE fixture and exercises music/rendering,
Creator compile/fix/install, Lua preview/install/run, revision conflicts,
Undo, locks, failed atomic edits, multiple clients, disconnect, revocation,
file publication and document closure.

Optional environment variables for the integration check:

- `DAW_MCP_SHOTS`: output directory for dark/light English/Russian Settings
  screenshots and the optional Codex client transcript.
- `VLT_MCP_CODEX_CLIENT`: absolute path to an installed Codex CLI executable.
  The fixture launches it with an isolated MCP configuration, ephemeral session,
  and a read-only prompt that discovers tools and reads a fixture project. This
  additional check requires the CLI's existing authentication/model access.

Run the existing AI, Creator, scripting, project workspace/tab and audio realtime
tests alongside these checks. The macOS bundle installs/copies the bridge next
to the application executable and gives it the framework runtime search path;
macOS packaging and execution still require verification on a Mac.

Protocol reference: [MCP versioning](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning).
Client reference: [Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp?surface=cli).
