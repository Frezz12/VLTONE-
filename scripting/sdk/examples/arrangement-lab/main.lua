local music, synth = require("music"), require("synth")
local context = vlt.context.get()
local settings = vlt.storage.get()
local options = {bars=8,tempo=112,seed=2718,root=45,progression=1,swing=.14,density=.65,humanize=.016}
-- Persist musical preferences only; project identities are resolved on each run.
for key,value in pairs(options) do
    if type(settings[key])=="number" then options[key]=settings[key] end
end
options.bars=math.max(4,math.min(16,math.floor(options.bars/4)*4))
options.progression=math.max(1,math.min(3,math.floor(options.progression)))
options.tempo=math.max(70,math.min(160,options.tempo))
options.seed=math.max(1,math.min(99999,math.floor(options.seed)))
options.root=({[45]=45,[50]=50,[52]=52,[48]=48})[options.root] or 45
options.swing=math.max(0,math.min(.45,options.swing))
options.density=math.max(0,math.min(1,options.density))
options.humanize=math.max(0,math.min(.04,options.humanize))
local plan,overrides,lines = music.build(options), {}, {}
local audioTracks,midiTracks,mix = {},{},{}
local worker,hostJob,pollTimer,serial,light,playhead=nil,nil,nil,0,false,0
local gridLane,gridStep=1,1
local lightColors={"#87521b","#a83956","#566679","#245e9d","#246d53","#714995"}
local function laneColor(lane) return light and lightColors[lane] or music.colors[lane] end
local ready=false
local function set(id,value) value.id=id; vlt.ui.set(value) end
local function log(text)
    lines[#lines+1]=text
    if #lines>100 then table.remove(lines,1) end
    print(text)
    if ready then set("log",{value=table.concat(lines,"\n")});set("status",{text=text}) end
end
local function guarded(fn)
    return function(...)
        local ok,result=pcall(fn,...)
        if not ok then log("Ошибка: "..tostring(result)) end
        return result
    end
end
local function snapshot()
    local p=vlt.project.get{includeEvents=false}
    if p.trackCount and p.trackCount>#p.tracks then error("В проекте слишком много дорожек для обзора; откройте отдельную вкладку.") end
    return p
end
local function apply(label,operations,revision)
    if #operations==0 then return {created={}} end
    return vlt.project.apply{revision=revision or snapshot().revision,label="Arrangement Lab / "..label,operations=operations}
end
local function findTrack(p,name,known)
    local found
    for _,track in ipairs(p.tracks) do
        if known and track.id==known then return track end
        if track.name==name then
            if found then error("Две дорожки с именем "..name.."; переименуйте одну из них.") end
            found=track
        end
    end
    return found
end
local function tracks(kind,create)
    local p=snapshot()
    local cache=kind=="audio" and audioTracks or midiTracks
    local prefix=kind=="audio" and "[Lab] " or "[Lab MIDI] "
    local operations={}
    for lane=1,6 do
        local t=findTrack(p,prefix..music.names[lane],cache[lane])
        if t then cache[lane]=t.id
        elseif create then operations[#operations+1]={op="track.add",name=prefix..music.names[lane],kind=kind} end
    end
    if #operations>0 then apply("Подготовить дорожки",operations,p.revision);return tracks(kind,false) end
    local result={}
    for lane=1,6 do result[lane]=findTrack(p,prefix..music.names[lane],cache[lane]) end
    return result,p.revision
end
local function ownedClip(track,prefix)
    local found
    for _,clip in ipairs(track.clips) do
        -- The host preserves scratch imports under UUID-prefixed filenames.
        -- Recognize that exact prefix after saving/reopening, too.
        local name=clip.name
        local uuid,original=name:match("^([%x%-]+)%-(.+)$")
        if uuid and #uuid==36 then name=original end
        if name:sub(1,#prefix)==prefix then
            if found then error("Несколько клипов Lab на "..track.name.."; выберите отдельную дорожку.") end
            found=clip
        end
    end
    return found
end
local function drawArrangement()
    if not ready then return end
    local size=vlt.ui.get{id="arrangement"}
    local w,h=size.width,size.height
    local left,top=70,26
    local laneHeight=(h-top-14)/6
    local scale=(w-left-12)/plan.beats
    local drawing=vlt.array({})
    for bar=0,plan.bars do
        local x=left+bar*4*scale
        drawing[#drawing+1]={type="line",x=x,y=top-4,x2=x,y2=h-8,color=light and "#cbd5dc" or "#2b3b4e"}
        if bar<plan.bars then drawing[#drawing+1]={type="text",x=x+4,y=17,text=tostring(bar+1),color=light and "#526477" or "#a9b7c9"} end
    end
    for lane,data in ipairs(plan.lanes) do
        local y=top+(lane-1)*laneHeight
        drawing[#drawing+1]={type="text",x=8,y=y+laneHeight*.57,text=data.name,color=laneColor(lane)}
        for _,note in ipairs(data.notes) do
            local pitchOffset=lane>=4 and (note.pitch%24)/24*(laneHeight*.58) or 0
            drawing[#drawing+1]={type="rect",x=left+note.start*scale,y=y+laneHeight*.68-pitchOffset,
                w=math.max(2,note.length*scale-1),h=lane>=4 and 4 or math.max(4,laneHeight*.30),fill=true,color=laneColor(lane)}
        end
    end
    if playhead>0 and playhead<plan.beats then
        local x=left+playhead*scale
        drawing[#drawing+1]={type="line",x=x,y=top-4,x2=x,y2=h-8,color=light and "#1d624d" or "#ffffff",width=2}
    end
    set("arrangement",{drawing=drawing})
end
local function drumOn(lane,step)
    if overrides[lane] and overrides[lane][step]~=nil then return overrides[lane][step] end
    for _,note in ipairs(plan.lanes[lane].notes) do
        if note.start<4 and math.floor(note.start*4+.15)+1==step then return true end
    end
    return false
end
local function drawGrid()
    if not ready then return end
    local size=vlt.ui.get{id="drumGrid"}
    local cell=(size.width-88)/16
    local row=(size.height-40)/3
    local drawing=vlt.array({})
    for step=1,16 do drawing[#drawing+1]={type="text",x=76+(step-1)*cell,y=20,
        text=tostring(step),color=light and "#526477" or "#a9b7c9"} end
    for lane=1,3 do
        drawing[#drawing+1]={type="text",x=8,y=40+(lane-.5)*row,text=music.names[lane],color=light and "#25364c" or music.colors[lane]}
        for step=1,16 do
            drawing[#drawing+1]={type="rect",x=74+(step-1)*cell,y=32+(lane-1)*row,w=math.max(2,cell-5),h=row-8,
                color=drumOn(lane,step) and laneColor(lane) or (light and "#dce4eb" or "#263447"),fill=true}
        end
    end
    drawing[#drawing+1]={type="rect",x=72+(gridStep-1)*cell,y=30+(gridLane-1)*row,w=cell-1,h=row-4,
        color=light and "#243548" or "#edf3f9",fill=false,width=2}
    set("drumGrid",{drawing=drawing})
end
local function rebuild()
    plan=music.build(options,overrides);music.verify(plan)
    if ready then
        set("summary",{text=string.format("%d тактов   ·   %d нот   ·   %.1f сек   ·   seed %d",plan.bars,plan.count,plan.beats*60/plan.tempo,options.seed)})
        set("seed",{value=options.seed})
        set("grooveInfo",{text=string.format("Swing %.0f%%   /   Плотность %.0f%%",options.swing*100,options.density*100)})
        drawArrangement();drawGrid()
    end
end
local function refresh()
    if not ready then return end
    local p=snapshot()
    local rows=vlt.array({})
    local kinds={[0]="Аудио",[1]="Инструмент",[2]="MIDI",[3]="Паттерн",[4]="Автоматизация",[5]="Шина"}
    for _,t in ipairs(p.tracks) do rows[#rows+1]={t.name,kinds[t.kind] or tostring(t.kind),tostring(#t.clips),tostring(#t.devices)} end
    set("projectTable",{rows=rows})
    set("projectInfo",{text=string.format("Проект: %s   ·   %d дорожек   ·   revision %d",p.name or "VLTONE",#p.tracks,p.revision)})
end
local function applyMidi()
    assert(not worker,"Сначала дождитесь сборки или отмените её")
    vlt.transport.set{tempo=plan.tempo}
    local ts,revision=tracks("midi",true)
    local create={}
    for lane,t in ipairs(ts) do
        if not ownedClip(t,"Lab MIDI ") then create[#create+1]={op="clip.addMidi",track=t.id,start=0,length=plan.beats} end
    end
    local added=apply("Создать MIDI-клипы",create,revision).created
    local p=snapshot()
    local operations,index={},1
    for lane=1,6 do
        local t=findTrack(p,"",midiTracks[lane])
        local clip=ownedClip(t,"Lab MIDI ")
        local id=clip and clip.id or added[index]
        if not clip then index=index+1 end
        operations[#operations+1]={op="clip.rename",track=t.id,clip=id,name="Lab MIDI "..music.names[lane]}
        operations[#operations+1]={op="clip.trim",track=t.id,clip=id,start=0,length=plan.beats,offset=0}
        operations[#operations+1]={op="notes.set",track=t.id,clip=id,notes=plan.lanes[lane].notes}
    end
    vlt.project.apply{revision=p.revision,label="Arrangement Lab / Применить MIDI",operations=operations}
    log("Создано "..plan.count.." MIDI-нот; редактирование нот — один шаг Undo.")
    refresh()
end
local function busy(value)
    for _,id in ipairs({"midi","build","variation","analyze","applyMix","savePreset"}) do set(id,{enabled=not value}) end
    set("cancel",{enabled=value})
    set("controls",{enabled=not value})
    set("rhythmControls",{enabled=not value})
    set("drumGrid",{enabled=not value})
end
local function publish(paths,frozen)
    local ts,revision=tracks("audio",true)
    assert(vlt.transport.get().tempo==frozen.tempo,"Темп изменился во время синтеза; повторите сборку.")
    local operations={}
    for lane,t in ipairs(ts) do
        local previous=ownedClip(t,"Lab_"..music.names[lane].."_")
        if previous then operations[#operations+1]={op="clip.remove",track=t.id,clip=previous.id} end
        operations[#operations+1]={op="clip.importAudio",track=t.id,path=paths[lane],start=0}
        operations[#operations+1]={op="track.mix",track=t.id,volume=mix[lane].volume,pan=mix[lane].pan,muted=mix[lane].muted}
    end
    vlt.project.apply{revision=revision,label="Arrangement Lab / Опубликовать 6 стемов",operations=operations}
    set("progress",{value=1});busy(false)
    log(string.format("Готово: 6 WAV-стемов / %.1f сек / 24 кГц. Публикация — один шаг Undo.",frozen.beats*60/frozen.tempo+.6))
    refresh()
end
local function buildAudio()
    assert(not worker and not hostJob,"Уже выполняется задача")
    applyMidi()
    local frozen=plan
    serial=serial+1;busy(true);set("progress",{value=0})
    log("Синтез 6 стемов: файлы появятся в проекте после завершения всей сборки.")
    local ok,started=pcall(synth.start,frozen,context,serial,function(value,lane)
        set("progress",{value=value})
        set("status",{text=string.format("Синтез: %s   ·   %.0f%%",music.names[lane],value*100)})
    end,function(paths)
        worker=nil
        local ok,err=pcall(publish,paths,frozen)
        if not ok then busy(false);log("Публикация отклонена: "..tostring(err)) end
    end,function(err) worker=nil;busy(false);log("Ошибка синтеза: "..err) end)
    if not ok then busy(false);error(started) end
    worker=started
end
local function cancel()
    if worker then worker.cancel();worker=nil end
    if hostJob then vlt.jobs.cancel{id=hostJob};hostJob=nil end
    if pollTimer then vlt.timers.stop{id=pollTimer};pollTimer=nil end
    busy(false);set("progress",{value=0});log("Задача отменена. Незавершённые стемы не добавлены в проект.")
end
local function inspect()
    local count=music.verify(plan)
    local rows={{"Генератор","OK",tostring(count).." корректных нот"}}
    local total=0
    for _,track in ipairs(tracks("midi",false)) do
        local clip=ownedClip(track,"Lab MIDI ")
        if clip then
            local p=vlt.project.get{track=track.id,clip=clip.id}
            local actual=p.tracks[1].clips[1]
            total=total+#actual.notes
            rows[#rows+1]={track.name,"OK",tostring(#actual.notes).." нот в проекте"}
        end
    end
    rows[#rows+1]={"Сверка MIDI",total==count and "OK" or "ОЖИДАНИЕ",tostring(total).." / "..count}
    set("checks",{rows=rows});log("Проверено: "..count.." нот в плане; "..total.." в проекте.")
    return total
end
local function analyze()
    assert(not hostJob and not worker,"Уже выполняется задача")
    serial=serial+1
    local path=context.temporaryDirectory.."/mix-"..serial..".wav"
    local phase="render"
    hostJob=vlt.audio.render{path=path,start=0,["end"]=plan.beats+.6*plan.tempo/60}
    if type(hostJob)=="table" then hostJob=hostJob.id end
    busy(true);log("Рендер снимка проекта для измерений…")
    pollTimer=vlt.timers.start{milliseconds=150,callback=function()
        local ok,err=pcall(function()
            local job=vlt.jobs.get{id=hostJob}
            set("progress",{value=(phase=="render" and 0 or .5)+(job.progress or 0)*.5})
            if not job.complete then return end
            if job.error and job.error~="" then error(job.error) end
            if phase=="render" then
                hostJob=vlt.audio.process{operation="analyze",path=path}
                if type(hostJob)=="table" then hostJob=hostJob.id end
                phase="analyze";log("Измерение фактически отрендеренного звука…")
                return
            end
            local result=job.result
            local db=function(value) return value>0 and string.format("%.2f dBFS",20*math.log(value,10)) or "Тишина" end
            local rows={
                {"Длительность",string.format("%.2f с",result.frames/result.sampleRate)},
                {"Пик",db(result.peak)},{"RMS",db(result.rms)},
                {"Частота",tostring(result.sampleRate).." Гц"},{"Каналы",tostring(result.channels)},
                {"Первая атака",string.format("%.2f мс",result.firstSoundFrame/result.sampleRate*1000)}
            }
            set("measurements",{rows=rows})
            vlt.timers.stop{id=pollTimer};pollTimer=nil;hostJob=nil
            set("progress",{value=1});busy(false);log("Аудио измерено. WAV: "..path)
        end)
        if not ok then cancel();log("Ошибка анализа: "..tostring(err)) end
    end}
end
local dark={background="#101721",surface="#192432",field="#111c28",text="#edf3f9",muted="#a9b8cb",accent="#7ae0bc",onAccent="#112a21",border="#33465b"}
local bright={background="#f0f3f5",surface="#ffffff",field="#e7edf2",text="#243548",muted="#53677d",accent="#226c53",onAccent="#ffffff",border="#b9c9d5"}
local function button(id,text,fn,primary)
    return {type="button",id=id,text=text,style=primary and "primary" or "secondary",onClick=guarded(fn)}
end
local function changed(key)
    return guarded(function(e) options[key]=e.value;rebuild() end)
end
for lane,data in ipairs(plan.lanes) do mix[lane]={volume=data.volume,pan=data.pan,muted=false} end
local mixerRows={}
for lane=1,6 do
    local index=lane
    mixerRows[#mixerRows+1]={type="row",style="card",spacing=14,children={
        {type="label",id="mixerName"..lane,text=music.names[lane],width=80,style={color=music.colors[lane],fontWeight=600}},
        {type="slider",id="gain"..lane,label=music.names[lane].." — уровень",min=0,max=1,value=mix[lane].volume,stretch=1,onChange=function(e) mix[index].volume=e.value end},
        {type="number",label="Pan",min=-1,max=1,step=.05,decimals=2,value=mix[lane].pan,width=150,onChange=function(e) mix[index].pan=e.value end},
        {type="toggle",text="Mute",id="mute"..lane,value=false,onChange=function(e) mix[index].muted=e.value end}
    }}
end
mixerRows[#mixerRows+1]=button("applyMix","Применить микс",function()
    local operations={}
    local ts,revision=tracks("audio",false)
    for lane,t in ipairs(ts) do
        operations[#operations+1]={op="track.mix",track=t.id,volume=mix[lane].volume,pan=mix[lane].pan,muted=mix[lane].muted}
    end
    assert(#operations>0,"Сначала соберите аудио")
    apply("Микшер",operations,revision);log("Микс применён одним шагом Undo.")
end,true)
mixerRows[#mixerRows+1]={type="label",text="Уровни здесь — черновик. «Применить микс» записывает все каналы атомарно.",style="muted"}
vlt.ui.window{id="lab",title="Arrangement Lab · VLTONE",width=1120,height=850,theme=dark,
style={padding=22,fontSize=13},styles={
    title={fontSize=28,fontWeight=700},eyebrow={fontSize=11,fontWeight=600,color="$accent"},muted={color="$muted"},
    card={background="$surface",radius=10,borderColor="$border",borderWidth=1,padding=14},
    primary={background="$accent",color="$onAccent",fontWeight=600,padding=10,radius=7,hover={background="$text",color="$background"}},
    secondary={background="$surface",color="$text",borderColor="$border",borderWidth=1,padding=8,radius=7,hover={borderColor="$accent"}}
},children={
    {type="row",children={
        {type="column",stretch=1,spacing=2,children={
            {type="label",text="VLTONE  /  COMPOSITION WORKBENCH",style="eyebrow"},
            {type="label",text="Arrangement Lab",style="title",wrap=false}
        }},
        button("theme","Светлая / тёмная",function()
            light=not light;set("lab",{theme=light and bright or dark})
            for lane=1,6 do set("mixerName"..lane,{style={color=laneColor(lane),fontWeight=600}}) end
            drawArrangement();drawGrid()
        end)
    }},
    {type="label",id="summary",text="",style="muted",wrap=false},
    {type="tabs",id="pages",stretch=1,children={
        {type="column",title="Композиция",spacing=12,children={
            {type="column",id="controls",style="card",spacing=10,children={
                {type="row",spacing=12,children={
                    {type="choice",label="Тональность",id="key",items={"A minor","D minor","E minor","C minor"},value=({[45]=1,[50]=2,[52]=3,[48]=4})[options.root],stretch=1,
                        onChange=guarded(function(e) options.root=({45,50,52,48})[e.value];rebuild() end)},
                    {type="choice",label="Гармония",items={"i – VI – iv – VII","i – iv – VII – VI","i – VII – VI – v"},value=options.progression,stretch=2,onChange=changed("progression")},
                    {type="number",label="Такты",min=4,max=16,step=4,decimals=0,value=options.bars,width=135,onChange=changed("bars")},
                    {type="number",label="BPM",min=70,max=160,step=1,decimals=0,value=options.tempo,width=140,onChange=changed("tempo")},
                    {type="number",label="Seed",id="seed",min=1,max=99999,decimals=0,value=options.seed,width=160,onChange=changed("seed")}
                }},
                {type="row",children={
                    {type="label",text="Параметры меняют план. Сборка аудио применяет MIDI, темп и создаёт шесть стемов.",style="muted",stretch=1},
                    button("variation","Новая вариация",function() options.seed=options.seed%99999+1;rebuild();log("Новая вариация: seed "..options.seed) end)
                }}
            }},
            {type="canvas",id="arrangement",label="План композиции: шесть партий по тактам",minHeight=225,stretch=1,
                style={background="$field",radius=8},onResize=drawArrangement},
            {type="row",children={
                button("midi","Применить MIDI",applyMidi),
                button("build","Собрать 6 аудиостемов",buildAudio,true),
                {type="spacer",stretch=1},
                button("savePreset","Сохранить настройки",function() vlt.storage.set{value=options};log("Музыкальные настройки сохранены.") end)
            }}
        }},
        {type="column",title="Ритм",spacing=16,children={
            {type="label",text="16 шагов · мышь или стрелки + пробел. R — сброс. Рисунок повторяется во всех тактах.",style="muted"},
            {type="canvas",id="drumGrid",label="Шаговый секвенсор. Стрелки: выбрать шаг. Пробел: переключить. R: сбросить.",minHeight=220,stretch=1,
                style={background="$field",radius=8},onResize=drawGrid,
                onPointer=guarded(function(e)
                    if e.value.phase~="down" then return end
                    local size=vlt.ui.get{id="drumGrid"}
                    local step=math.floor((e.value.x-74)/((size.width-88)/16))+1
                    local lane=math.floor((e.value.y-32)/((size.height-40)/3))+1
                    if lane<1 or lane>3 or step<1 or step>16 then return end
                    gridLane,gridStep=lane,step
                    overrides[lane]=overrides[lane] or {};overrides[lane][step]=not drumOn(lane,step);rebuild()
                end),
                onKey=guarded(function(e)
                    local key=e.value.key
                    if key=="Left" then gridStep=math.max(1,gridStep-1) end
                    if key=="Right" then gridStep=math.min(16,gridStep+1) end
                    if key=="Up" then gridLane=math.max(1,gridLane-1) end
                    if key=="Down" then gridLane=math.min(3,gridLane+1) end
                    if key=="Space" or key=="Return" then
                        overrides[gridLane]=overrides[gridLane] or {}
                        overrides[gridLane][gridStep]=not drumOn(gridLane,gridStep)
                    end
                    if key=="R" then overrides={} end
                    rebuild()
                end)},
            {type="column",id="rhythmControls",style="card",children={
                {type="label",id="grooveInfo",text=""},
                {type="row",spacing=16,children={
                    {type="slider",label="Swing",min=0,max=.45,value=options.swing,stretch=1,onChange=changed("swing")},
                    {type="slider",label="Плотность",min=0,max=1,value=options.density,stretch=1,onChange=changed("density")},
                    {type="slider",label="Humanize",min=0,max=.04,value=options.humanize,stretch=1,onChange=changed("humanize")}
                }},
                button("resetGrid","Сбросить ручной ритм",function() overrides={};rebuild();log("Ритм сброшен к генератору.") end)
            }}
        }},
        {type="scroll",title="Микшер",children=mixerRows},
        {type="column",title="Проверки",spacing=10,children={
            {type="row",children={
                button("inspect","Проверить MIDI",inspect),button("analyze","Рендер + измерения",analyze,true),
                button("refresh","Обновить проект",refresh)
            }},
            {type="splitter",orientation="horizontal",stretch=1,children={
                {type="table",id="checks",columns={"Проверка","Статус","Детали"},rows=vlt.array({}),minHeight=130},
                {type="table",id="measurements",columns={"Параметр аудио","Значение"},rows=vlt.array({}),minHeight=130}
            }},
            {type="label",id="projectInfo",text="",style="muted"},
            {type="table",id="projectTable",columns={"Дорожка","Тип","Клипы","Устройства"},rows=vlt.array({}),height=155},
            {type="textEdit",id="log",label="Журнал выполнения",readOnly=true,value="",height=100}
        }}
    }},
    {type="progress",id="progress",value=0,height=20},
    {type="row",children={
        {type="label",id="status",text="Готово. План не изменяет проект до применения.",style="muted",stretch=1},
        button("cancel","Отменить задачу",cancel),
        button("play","Слушать",function() vlt.transport.set{beat=0,playing=true} end),
        button("stop","Стоп",function() vlt.transport.set{playing=false};playhead=0;drawArrangement() end)
    }}
}}
ready=true;rebuild();refresh();busy(false)
vlt.events.on{event="project",milliseconds=350,callback=guarded(refresh)}
vlt.events.on{event="transport",milliseconds=100,callback=guarded(function(e)
    playhead=e.beat or 0
    if e.playing then drawArrangement() end
end)}
log("Готово: 6 партий. Примените MIDI или соберите аудио.")
if (context.arguments or {}).autoBuild then guarded(buildAudio)() end
return {window="lab",notes=plan.count,bars=plan.bars,seed=options.seed}
