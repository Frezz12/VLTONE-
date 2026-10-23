-- Custom algorithm in Lua, one bounded PCM block per callback.
local stream, timer, frame, crossings, previous = nil, nil, 0, 0, 0
local function stop()
    if timer then vlt.timers.stop{id=timer}; timer=nil end
    if stream then vlt.audio.close{id=stream.id}; stream=nil end
end
vlt.ui.window{title="Custom PCM analysis", children={
    {type="button", text="Choose audio", onClick=function()
        stop()
        local path=vlt.ui.chooseFile{mode="open", filter="Audio (*.wav *.flac *.aiff)"}
        if not path then return end
        stream=vlt.audio.open{path=path}; frame=0; crossings=0; previous=0
        timer=vlt.timers.start{milliseconds=30, callback=function()
            local info, pcm=vlt.audio.read{id=stream.id, frame=frame, frames=2048}
            for sample=1, #pcm, info.channels do
                local value=pcm[sample]
                if (value>0 and previous<=0) or (value<0 and previous>=0) then crossings=crossings+1 end
                previous=value
            end
            frame=frame+info.frames
            vlt.ui.set{id="progress", value=frame/math.max(1,stream.frames)}
            if frame>=stream.frames or info.frames==0 then
                local seconds=stream.frames/stream.sampleRate
                vlt.ui.set{id="result", text=string.format("%d crossings; %.2f crossings/second", crossings, crossings/math.max(.001,seconds))}
                stop()
            end
        end}
    end},
    {type="button", text="Cancel", onClick=stop},
    {type="progress", id="progress", value=0},
    {type="label", id="result", text="Counts sign changes in the first channel, without loading the entire file."}
}}
