# VLTONE SDK 1 — guide for extension authors and AI

Read api.json, API.html and the runnable directories in examples. The installed
vlt.lua contains editor annotations generated from the same catalog as runtime validation.
Lua 5.5.1 is embedded. There is no reaper API and no native module loader.

1. Read current Creator sources and manifest before editing; use the expected
   Creator revision. Changes use the same Undo history as manual edits.
2. Choose narrow permissions. Preserve stable extension/action IDs across updates.
   Declared project.read/files.read are granted automatically unless explicitly revoked.
   Edits, writes, network and external programs still need approval. Paths remain scoped
   by the host; a “safe” label never bypasses these checks.
3. Read vlt.context.get() and honor target before global selection. The target can
   differ from selected clips/notes. Store IDs, never positions in a track list.
4. Read vlt.project.get(), prepare operations, then use vlt.project.apply with the
   snapshot's revision. A stale revision is an error: refresh and recompute.
   Snapshots are paged and limited to 1.5 MiB. Filter by track/clip, skip events
   when they are unnecessary and compare revision across pages. Use notes.update
   to edit a subset; notes.set replaces the entire clip's notes. Preserve automation
   shape, curve and eventOrder. parameters.list and devices.list accept offset/limit;
   parameters.list can filter by parameter ID.
5. Musical time is quarter-note beats, file time is frames plus explicit sample rate,
   parameters use native units. Use vlt.time.convert for explicit conversions.
   Parameter handles include track, device, parameter and device identity.
   Audio clip.source is the absolute source path. start/length/offset are beats.
   Inspect reversed/looped/stretched/composite before assuming a direct source mapping.
   clip.split takes absolute at beats: original id stays LEFT, created contains the new
   RIGHT id. Multiple cuts of one original id go RIGHT-TO-LEFT in one project.apply.
6. Sequential Lua tables become arrays. Use vlt.array({}) for an empty array.
   Returned arrays retain their array type when emptied.
7. Native widgets use table descriptions and Lua callbacks. A bind object containing
   track/device/parameter/identity synchronizes controls with one Undo per gesture.
   Never keep an Undo group open across callbacks.
8. Long work uses host jobs or bounded PCM blocks with timers/events. audio.read
   returns metadata and a buffer; #buffer is interleaved sample count, one-based.
   audio.write({id=output.id}, buffer) appends it. Publish with audio.close using
   publish=true. Existing destination files are never replaced.
9. Linked project media is automatically readable with project.read and files.read:
   use clip.source from project.get, or project.files{offset=0,limit=256}. The latter
   returns {files={ {path,name,bytes}, ... },total,offset,revision}. Page by total and
   compare revision. Sources are read-only; neighboring unreferenced files are not granted.
   Outside resources use ui.chooseFile. Packaged resources are under the
   context.resourceDirectory. Every run can read/write its context.temporaryDirectory
   with the appropriate files permission. Import a completed scratch file using
   project.apply clip.importAudio; the host preserves a durable copy for the project/Undo.
   Keep exports in a chosen destination if they must outlive this run without import.
   Preview has a disposable context.temporaryDirectory;
   it cannot access user files or launch external processes.
   The resources/ directory is reserved for data; even .lua files there are resources.
10. Check all sources without execution, preview on a copy, inspect logs/results and
    fix failures. A real-project run is separate. Installation does not execute code.
    Respect the user's requested install/export scope.

Each helper has a 64 MiB Lua allocator and a two second callback execution budget;
time spent waiting for broker responses is excluded. A tool stays alive while it owns
windows, subscriptions or running jobs. Closing its last window ends it unless the
command declares background=true. Stop kills a busy helper. A crashed autostart
extension is suspended until the user re-enables it.

Host commands enforce availability and assistant mode. Extensions cannot modify
shared projects, including through commands or parameter bindings. Reads and independent
file operations remain available with granted permissions.

CLI: vlt_script_tool --check PATH accepts Lua, Creator projects, directories and .vltscript.
vlt_script_tool --pack DIRECTORY OUTPUT.vltscript validates and exports.
Neither operation executes extension code. vlt_script_worker --sdk prints the catalog.


UI and workspace integration:
- Read UI_EN.html / UI_RU.html and examples/interactive-workbench. Use the catalog's
  widgets, widgetCommon, uiStyle, canvasEvents and workspaceViews entries; do not invent widget types.
- vlt.ui.window creates a modeless window, or a dock with dock=left/right/top/bottom.
  IDs are local to the instance. vlt.ui.get reads state; ui.set changes state without
  callbacks; ui.replace validates a whole new tree before replacing live controls.
- Use grid/row/column/group/tabs/scroll/splitter for layout, table/textEdit/image for data,
  and canvas onPointer/onWheel/onKey/onResize for custom interaction. Event payloads
  are {id,value}; indices are one-based. Give controls labels and visible focus.
- workspace.get reads panel/selection state. workspace.open navigates by stable view
  name and validated track/clip/device IDs. Plugin opening requires device identity.
  workspace.select validates all targets before changing local selection. Navigation
  requires the pinned project to be active and is blocked in preview; it never edits
  document data. Use project.apply/parameters.set for document changes.
- commands.list reports callable and available separately; commands.run still checks
  project.write, collaboration, AI mode and the active pinned project. No synthetic
  mouse/key events or raw widget pointers are exposed. Third-party UI internals are
  not part of the API: use the parameters that the plugin exposes to its host.
- UI handlers must return quickly. Limit updates to changed controls; do not rebuild
  a large tree on every pointer move. Keep drawing coordinates in logical pixels.
- Design windows with theme colors and styles defined on ui.window. Apply a named
  style, a local object or {use="card",padding=24} to any widget. The uiStyle catalog
  lists allowed properties, tokens and limits; raw CSS/QSS and URLs are rejected.
  Use STYLE_RU.html / STYLE_EN.html and examples/styled-workbench for a runnable design.
- ui.set replaces style/theme overrides, preserving values and callback bindings.
  Empty {} resets overrides; ui.replace inherits definitions and theme unless supplied.
  Invalid ui.set arguments leave the live controls untouched. Use palette tokens for
  adaptive color, maintain text contrast and label custom canvas interactions.
- For clip analysis use audio.renderClip, poll jobs.get (or subscribe to job), and
  read result.path with audio.open/read. Frame zero is the audible clip start,
  including Warp, stretch, reverse, looping, trims and comps, before live FX.
  Convert frames with result.start + frame / sampleRate / result.secondsPerBeat.
  Slice with project.sliceAudio{snapshot=result.snapshot,cuts=timelineBeats,label=...}.
  This edits only the captured clip, rejects stale revisions and creates one Undo.
  It sorts cuts and skips duplicates or segments at most 20 ms long. All fragments
  get new IDs in result.created. Warp, pitch-preserving stretch, loops, comps and
  fades are baked into durable media so cut boundaries do not reset DSP phase or
  envelopes. Undo restores all original settings. Simple source trims and Resample
  retain source edits. Original source files are never overwritten.
  Do not guess source-to-timeline mappings or reject stretched/looped clips.
