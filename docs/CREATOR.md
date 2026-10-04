# Creator

For C++ authoring, see the complete [Creator SDK reference](../creator/sdk/CREATOR_SDK_RU.md)
and [AI context guide](../creator/sdk/AI_GUIDE_RU.md). The SDK kit includes the
public header and connected, executable examples.

Open **View → Creator**. Creator has its own project and Undo history.
The welcome dialog offers a new project, existing/recent projects and recovery
of an unsaved draft. New projects contain Input, Output and Interface.

Open `resources/creator/Gentle Tremolo.vltcreator` for a working example:
Interface controls the LFO rate and the dry/wet Mix depth. The LFO modulates
Gain, and Mix combines it with the original stereo signal. Depth zero is dry.
The matching shareable module is
`resources/mini-modules/creator-gentle-tremolo.vltmini`.

## Editing

- Double-click a node in the searchable library, or press Tab on the canvas.
- Drag an output to an input of the same type. Audio is a green circle,
  Number a blue diamond and Gate a purple square. Incompatible targets dim.
- Select a node to connect inputs using native combo boxes in the properties
  panel; this is also the keyboard alternative to dragging wires.
- A connected parameter uses the incoming sample stream. Disconnecting restores
  its stored manual value.
- Drag node headers; marquee selects a group. Delete removes selected nodes or
  wires. Input, Output and Interface stay in the graph.
- Ctrl+C/Ctrl+V copy nodes and their internal connections; Ctrl+D duplicates.
  IDs are regenerated, so copies cannot steal another node's connections.
- Middle-drag or Space+drag pans. Ctrl+wheel zooms around the pointer; F fits
  the graph and Ctrl+0 restores 100%.
- Ctrl+S saves. Ctrl+Enter compiles.

Interface edits the module name, card material, background color or embedded
image, and up to two external controls. Each control has a stable ID, name,
unit, range, default, scale and appearance. Its output may feed several nodes.
The 100 px preview uses the production Channel Strip card.
The optional mode menu supports up to eight named graphs with shared controls
and card appearance. Adding a mode copies the current graph.

## Files and compilation

.vltcreator v2 stores editable graphs, C++ source, signatures, positions,
current mode, viewport and code-editor state; v1 projects remain readable.
Incomplete graphs can be saved. Recovery drafts are written atomically to
the application data directory. Save/Discard/Cancel applies when closing
Creator or the application.

Compile validates every mode and prepares DSP on a worker. It atomically
installs .vltmini v4 in Documents/VLTONE/MiniModules. Share that file;
recipients put it in their MiniModules folder or use the rack's Import action.
Images are embedded. Compilation does not produce a native VST binary.

Recompiling keeps the module ID and updates every matching card in the current
open project, including Master, as one project Undo action. Slot IDs, bypass,
pre/post-FX placement, seeds, matching parameter values and automation addresses
remain. Removed controls' automation lanes stay in the project.
Closed projects retain their embedded module definitions.
An offline export/Freeze defers application until it finishes.

At equal latency a new prepared graph crossfades with the previous instance
over 20 ms. A changed latency fades the old instance down before publishing the
new graph and its PDC, then fades up. The audio thread never constructs or
destroys these graphs. Invalid graphs and failed preparation keep the installed
file and live processors; a failed installation restores any pending fade.
No multiplayer protocol changes are part of Creator.

## Typed graphs

Typed graphs retain the existing mini-module structure and add stable source
and destination port IDs on each connection. Legacy v1/v2 graphs keep their
audio-only executor. Typed-graph validation rejects unknown versions/types/ports,
incompatible types, multiple producers for one input, cycles and missing
required audio inputs. Limits are 64 logical nodes, 128 wires and two external
controls. Unused nodes are retained in the editor and pruned from execution.

MiniNodeRegistry describes ports, ranges, defaults, units and modulation
eligibility. CreatorDspRuntime lowers the typed graph into AudioGraph and
executes it with GraphProcessor(1). Number and Gate streams use prepared
sample buffers and graph dependencies, including latency alignment with audio.
Interface outputs are separate source nodes; duplicate fan-out to the same
consumer shares the graph edge while retaining its distinct input bindings.

Math and control operations run per sample. Division by zero yields zero;
non-finite intermediate values are contained. Effect adapters use preallocated
sample-offset parameter events and existing DSP cores. Structural parameters
are static. EQ Band enables one zero-latency EQ band. Reverb uses streaming
Schroeder comb/allpass storage, prepared off the audio thread. Oscillators use
band-limited edge corrections; Random/Noise use deterministic local PRNG state.
v4 adds portable C++ Function nodes and separately checked callable dependencies.
MIDI, polyphony, feedback wires and AI authoring are future work.

DAW projects write format 15 and Channel Strip presets format 5. Older formats
remain readable. Typed graphs require a Creator-capable build; readers that
support unavailable mini modules preserve an unsupported definition without
executing it. This build rejects future project versions.

## Checks

creator_graph_test exercises registry nodes at 44.1/48/96/192 kHz in mono
and stereo, allocation-free processing/reset, event offsets, macro fan-out,
parallel latency alignment, modes, invalid graphs, effect modulation and live
crossfades. --creator-check exercises the actual Qt editor, project files,
Undo, copy/paste, typed ports, the card preview, installation and project-wide
updates. CTest covers dark/light themes, English/Russian and 100/150/200% DPI.
creator_integration_test covers background preparation during playback,
project-wide updates and rollback, stable automation, files and presets,
wet/dry/source export, Freeze and latency-changing publication.

Verified on Windows Release, 2026-10-03: 38 selected CTest cases pass,
including 12 Creator UI cases and 12 Channel Strip UI cases. Both example
files also pass native graph validation, DSP preparation and execution.

`creator_graph_test --bench` measures 64 stereo channels with three effects
each at 48 kHz / 256 frames, executed serially. The measured averages were
27.620 ms for direct Color+Doubler+Chorus versus 28.697 ms through typed graphs
(3.9% overhead); three Colors were 73.828 versus 75.404 ms (2.1%). These are
single-thread comparison measurements, not a claim that 192 effects fit a
5.33 ms realtime callback. The outer DAW graph controls channel parallelism.
