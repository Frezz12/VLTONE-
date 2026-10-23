local context = vlt.context.get()
local target = context.target or {}
if not target.track or not target.clip then return {message="Select a MIDI clip or notes first"} end
local selected = {}
for _, id in ipairs(context.notes or {}) do selected[id] = true end
-- A clicked note outside the selection is the explicit target.
if target.note and target.note ~= "" and not selected[target.note] then
    selected = {[target.note] = true}
end
local onlySelected = next(selected) ~= nil
local semitones = (context.arguments or {}).semitones or 1
local changed, offset, revision = vlt.array({}), 0, nil
repeat
    local project = vlt.project.get{track=target.track, clip=target.clip, eventOffset=offset, eventLimit=1024}
    if revision and revision ~= project.revision then error("Project changed; retry the command") end
    revision = project.revision
    local track = project.tracks[1]
    local clip = track and track.clips[1]
    if not clip or clip.kind ~= "midi" then return {message="Select a MIDI clip or notes first"} end
    for _, note in ipairs(clip.notes) do
        if not onlySelected or selected[note.id] then
            note.pitch = math.max(0, math.min(127, note.pitch + math.floor(semitones)))
            changed[#changed+1] = note
            if #changed > 4096 then error("Select at most 4096 notes per command") end
        end
    end
    offset = offset + #clip.notes
until offset >= clip.noteCount
return vlt.project.apply{revision=revision, label="Transpose notes",
    operations={{op="notes.update", track=target.track, clip=target.clip, notes=changed}}}
