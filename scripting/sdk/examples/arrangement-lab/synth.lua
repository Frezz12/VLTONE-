-- One bounded block per timer callback, with no whole-file PCM allocation.
local M = {}
local pi2 = math.pi*2
function M.voice(note,rate,tempo)
    local start=math.floor(note.start*60/tempo*rate)
    return {start=start, length=note.length*60/tempo, frequency=440*2^((note.pitch-69)/12), velocity=note.velocity/127}
end
function M.sample(kind,voice,frame,rate,noise)
    local t=(frame-voice.start)/rate
    if t<0 then return 0 end
    local f=voice.frequency
    local x
    if kind==1 then
        if t>.52 then return nil end
        x=math.sin(pi2*(48*t+3.7*(1-math.exp(-t*33))))*math.exp(-t*10)
        x=x+noise*.10*math.exp(-t*160)
    elseif kind==2 then
        if t>.32 then return nil end
        x=(noise*.75+math.sin(pi2*185*t)*.30)*math.exp(-t*19)*(1-math.exp(-t*700))
    elseif kind==3 then
        if t>.10 then return nil end
        x=noise*.55*math.exp(-t*48)*(1-math.exp(-t*1500))
    else
        local release=kind==5 and .45 or .10
        if t>voice.length+release then return nil end
        local env=math.min(1,t/(kind==5 and .025 or .004))*math.exp(-t*(kind==6 and 6 or .35))
        if t>voice.length then env=env*math.max(0,1-(t-voice.length)/release) end
        if kind==4 then
            x=(math.sin(pi2*f*t)+.24*math.sin(pi2*f*2*t)+.07*math.sin(pi2*f*3*t))*.66*env
        elseif kind==5 then
            x=(math.sin(pi2*f*t)+.28*math.sin(pi2*f*2.003*t)+.12*math.sin(pi2*f*3*t))*.25*env
        else
            x=(math.sin(pi2*f*t)+.30*math.sin(pi2*f*2*t)*math.exp(-t*8))*.50*env
        end
    end
    return x*voice.velocity
end
function M.start(plan,context,serial,progress,complete,failed)
    local state={lane=1,frame=0,rate=24000,voices={},nextNote=1,paths={},cancelled=false,noise=plan.count+17}
    local seed=vlt.audio.open{path=context.resourceDirectory.."/resources/block.wav"}
    state.frames=math.floor((plan.beats*60/plan.tempo+.6)*state.rate)
    local function close()
        if state.timer then vlt.timers.stop{id=state.timer};state.timer=nil end
        if state.output then vlt.audio.close{id=state.output.id};state.output=nil end
        if seed then vlt.audio.close{id=seed.id};seed=nil end
    end
    function state.cancel() state.cancelled=true;close() end
    local function nextLane()
        state.frame=0;state.voices={};state.nextNote=1
        state.path=context.temporaryDirectory.."/Lab_"..plan.lanes[state.lane].name.."_"..serial..".wav"
        state.output=vlt.audio.create{path=state.path,sampleRate=state.rate,channels=1}
    end
    local opened,openError=pcall(nextLane)
    if not opened then close();error(openError) end
    state.timer=vlt.timers.start{milliseconds=30,callback=function()
        local ok,err=pcall(function()
            local count=math.min(8192,state.frames-state.frame)
            local info,pcm=vlt.audio.read{id=seed.id,frame=0,frames=count}
            assert(info.frames==count,"PCM seed is too short")
            local notes=plan.lanes[state.lane].notes
            for i=1,count do
                local frame=state.frame+i-1
                while state.nextNote<=#notes and notes[state.nextNote].start*60/plan.tempo*state.rate<=frame do
                    state.voices[#state.voices+1]=M.voice(notes[state.nextNote],state.rate,plan.tempo)
                    state.nextNote=state.nextNote+1
                end
                state.noise=(state.noise*48271)%2147483647
                local noise=state.noise/1073741823.5-1
                local sample=0
                for j=#state.voices,1,-1 do
                    local value=M.sample(state.lane,state.voices[j],frame,state.rate,noise)
                    if value==nil then table.remove(state.voices,j) else sample=sample+value end
                end
                pcm[i]=math.max(-.97,math.min(.97,sample))
            end
            vlt.audio.write({id=state.output.id},pcm)
            state.frame=state.frame+count
            progress(((state.lane-1)+state.frame/state.frames)/6,state.lane)
            if state.frame>=state.frames then
                vlt.audio.close{id=state.output.id,publish=true};state.output=nil
                state.paths[state.lane]=state.path
                if state.lane==6 then close();complete(state.paths) else state.lane=state.lane+1;nextLane() end
            end
        end)
        if not ok then close();failed(tostring(err)) end
    end}
    return state
end
return M
