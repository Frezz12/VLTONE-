# AI chat: interface and integration

The assistant remains a Qt Widgets panel and uses the existing controller tools, undo history, music analysis and parameter host. No new model service, SDK or dependency was added.

| Before | After |
|---|---|
| Rounded glass plate and message cards | Flat rectangular panel, composer and cards using the application palette |
| Selecting text was the only way to copy a message | Copy on each message; edit a request as a new draft; searchable conversation with copy-all and text export |
| Every transcript update scrolled to the bottom | Follow output only while reading the latest messages; explicit jump to latest |
| Focusing the multiline composer could disable GPU rendering because of Qt's text-caret composition | The small input viewport uses the existing native-control texture path; the workspace keeps GPU rendering |
| Main-window shortcuts could intercept chat actions | Delete/Backspace detach selected attachments; Find opens conversation search while focus is in the chat |
| Attach button only displayed an instruction | File/MIDI/folder picker, drag-and-drop, attach selected samples, compact attachment count, Delete/context-menu removal |
| Folder attachment unsupported | One row per folder; double-click to inspect up to 200 indexed files; AI search can target an attached folder ID |
| Only track/clip selection supplied | Visible selection summary, context inspector, sample usage, open plugin editors and piano-roll note selection with stable IDs |
| Browser defaults could grant Music/Downloads implicitly | Separate explicit AI library grants; collection entries and attached files/folders only; single-file grants do not scan siblings |
| Library refreshed for every prompt | Background index reused between prompts; explicit refresh in the attachment menu |
| Temporary provider error immediately ended a turn | Configurable 1–30 minute inactivity timeout (10 by default), 0–5 retries (3 by default), backoff and Retry-After |
| Truncated streams could lose prose or finish as success | Keep partial text, reject incomplete commands, preserve completed tool outcomes when continuing |
| Only LF-delimited SSE events decoded | CRLF/chunk-boundary handling; bounded tool indices and malformed-response errors |
| Full parameter list with rounded values | Search and pagination, exact plain-unit values, defaults/stepped metadata, requested/previous/actual values on changes |
| Default/detected progression only; extended chord tones could be dropped | Explicit chord progression, all supplied chord tones, chord changes inside a bar and note lengths limited to the next change |

Temporary failures are retried only before generated output. Ambiguous paid managed-provider attempts are not replayed automatically. Stop cancels the request and retry timer. Continuation retains prior tool results and the interaction mode, so completed edits are visible to the model.

Library paths stay in native code. Model-facing sample references remain opaque content IDs; folder labels and relative locations are supplied for navigation. Removing an attachment updates its grants and the active tool context. The collapsed attachment list remains collapsed across mode changes. In legacy preferences, automatic Music/Downloads roots are excluded unless explicitly added again or attached.

## Verification

Logs and screenshots are under `artifacts/ai-chat/`. Focused checks cover the protocol, explicit library scope, attachment controls, copy, scroll anchoring, music composition and parameter editing. Local HTTP fixtures simulate 503, SSE 429, empty replies, interrupted responses and cancellation; no live model requests are used.

- Four focused CTests pass: `ai_tools_test`, `project_music_context_test`, `content_catalog_test`, `composition_engine_test`.
- The CPU assistant selftest passes the local HTTP recovery fixtures, compact folder attachments, deduplication, stable attachment IDs, Delete, message copy, Find, scripted tool execution and undo/redo.
- Long-message checks use an unbroken 2,500-character word and a 100-line reply, allow Qt's nested layouts to settle, and verify scroll anchoring and viewport width. Wrapped labels retain their height-for-width policy.
- Russian screenshots were inspected at 240 px panel width on CPU and 320 px / 150% scale on GPU. A hidden-window GPU grab was blank; the normal screenshot runner with an exposed window produced the expected scene.
- The final packaged stage passes the focused assistant selftest on native Windows with GPU rendering at 100% and 150%, Russian locale and a clean runtime PATH. Both runs include the transport, library, attachment, keyboard, copy, scroll and undo checks, with no critical render warning or GPU fallback.
- The generic plugin-picker image check fails its rounded-corner assertion on native Windows before reaching the chat. It remains enabled in the full selftest and its dedicated command. `DAW_SELFTEST_AI_ONLY` now omits that unrelated prerequisite, while still rejecting critical Qt rendering warnings.
- Stage executable: `build-windows/stage/bin/VLTONE.exe`. SHA-256: `A3165138AB38D80DEFFCDF4EA2815E6BA6B617AB4821E5F1B42568BA92D17986`. Qt runtime deployment succeeded; the stage executable matches the frozen verification copy.

## Scope

- Musical context uses MIDI notes, stored audio analysis and the existing audio measurement tools. It does not give a text model human-like listening.
- Plugin editing uses parameters exposed by the host; arbitrary controls private to a vendor GUI remain outside this interface.
- The conversation remains in memory for the open panel; export is explicit. This change does not add an account-wide conversation archive.
- Provider availability and live free-model rate limits require a real account/model run to assess. Local fixtures verify the client recovery behavior.
- Concurrent slide-note development shares this checkout and can temporarily break a combined application build. Its source changes are preserved.
