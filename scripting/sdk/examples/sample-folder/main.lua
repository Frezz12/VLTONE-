local queue, output, current, done, total = {}, nil, nil, 0, 0
local function status(text) vlt.ui.set{id="status", text=text} end
local function nextFile()
    local file = table.remove(queue, 1)
    if not file then current=nil; status("Finished: "..done.." files"); return end
    current = vlt.audio.process{
        operation="trim", path=file.path, thresholdDb=-42,
        output=output.."/"..file.name:gsub("%.[^.]+$", "").."-trimmed.wav"
    }.id
end
vlt.events.on{event="job", callback=function(event)
    if event.id ~= current then return end
    if event.error ~= "" then queue={}; current=nil; status(event.error); return end
    done=done+1; vlt.ui.set{id="progress", value=done/math.max(1,total)}
    nextFile()
end}
vlt.timers.start{milliseconds=100, callback=function()
    if current then
        local job=vlt.jobs.get{id=current}
        vlt.ui.set{id="progress", value=(done+job.progress)/math.max(1,total)}
    end
end}
vlt.ui.window{id="main", title="Sample folder trimmer", children={
    {type="label", text="Trim outer silence to new WAV files. Originals are preserved."},
    {type="button", text="Choose folders and start", onClick=function()
        if current then return end
        local input=vlt.ui.chooseFile{mode="folder"}
        if not input then return end
        output=vlt.ui.chooseFile{mode="folder", write=true}
        if not output then return end
        queue={}
        for _, file in ipairs(vlt.files.list{path=input}) do
            local ext=file.name:lower():match("%.([^.]+)$")
            if not file.directory and (ext=="wav" or ext=="aiff" or ext=="flac") then queue[#queue+1]=file end
        end
        done=0; total=#queue; status("Processing "..total.." files"); nextFile()
    end},
    {type="button", text="Cancel", onClick=function()
        queue={}; if current then vlt.jobs.cancel{id=current}; current=nil end; status("Cancelled")
    end},
    {type="progress", id="progress", value=0},
    {type="label", id="status", text="Ready"}
}}
