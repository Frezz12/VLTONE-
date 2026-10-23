# VLTONE Script SDK 1

Lua 5.5.1 is bundled with VLTONE. Scripts run in isolated helper processes.
The host owns all project mutations, permissions and native windows.
Run `vlt_script_worker --sdk` to obtain the authoritative API catalog, or
`vlt_script_worker --check file.lua` to validate source without executing it.

`examples/arrangement-lab` is a complete composition workbench: deterministic
MIDI, interactive canvas, cancellable six-part PCM synthesis, native mixer,
project verification and measured audio previews. Its `verify` command checks
the composition model without editing a project.
