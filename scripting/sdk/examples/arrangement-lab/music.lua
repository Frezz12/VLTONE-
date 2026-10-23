-- Pure composition model. All musical positions are quarter-note beats.
local M = {}
M.names = {"Kick", "Snare", "Hats", "Bass", "Keys", "Arp"}
M.colors = {"#edb677", "#e58a9d", "#a7b4c9", "#80baff", "#80dfbd", "#bc9ee8"}
M.progressions = {{0,5,3,6}, {0,3,6,5}, {0,6,5,4}}
M.degrees = {0,2,3,5,7,8,10}
local function random(seed)
    return function()
        seed = (seed * 48271) % 2147483647
        return seed / 2147483647
    end
end
local function bounded(x,lo,hi) return math.max(lo,math.min(hi,x)) end
function M.build(options, overrides)
    local o = options
    local rand = random(o.seed)
    local plan = {bars=o.bars, beats=o.bars*4, tempo=o.tempo, lanes={}, count=0, chords={}}
    local volumes = {0.73,0.43,0.25,0.49,0.34,0.25}
    local pans = {0,0.06,-0.24,0,-0.18,0.28}
    for lane=1,6 do
        plan.lanes[lane] = {name=M.names[lane], color=M.colors[lane], notes={}, volume=volumes[lane], pan=pans[lane]}
    end
    local function add(lane,pitch,beat,length,velocity)
        local notes = plan.lanes[lane].notes
        local human = (rand()-.5)*o.humanize
        local start = bounded(beat+human,0,plan.beats-0.04)
        notes[#notes+1] = {id="lab-"..lane.."-"..(#notes+1), pitch=pitch, start=start,
            length=math.min(length,plan.beats-start), velocity=math.floor(bounded(velocity+(rand()-.5)*12,1,127))}
        plan.count=plan.count+1
    end
    for bar=0,o.bars-1 do
        local degree = M.progressions[o.progression][math.floor(bar/2)%4+1]
        local root = o.root+M.degrees[degree+1]
        local third = M.degrees[(degree+2)%7+1] + (degree+2>=7 and 12 or 0) - M.degrees[degree+1]
        local fifth = M.degrees[(degree+4)%7+1] + (degree+4>=7 and 12 or 0) - M.degrees[degree+1]
        local seventh = M.degrees[(degree+6)%7+1] + (degree+6>=7 and 12 or 0) - M.degrees[degree+1]
        local chord = {root+12,root+12+third,root+12+fifth,root+12+seventh}
        plan.chords[bar+1] = {root=root, pitches=chord, degree=degree+1}
        local intro = bar<2
        local lift = bar>=o.bars-2
        for step=0,15 do
            local beat = bar*4+step/4+(step%2==1 and o.swing*.25 or 0)
            local hits = {step%4==0 or (lift and step==14), step==4 or step==12,
                step%2==0 or (not intro and rand()<o.density)}
            for lane=1,3 do
                if overrides and overrides[lane] and overrides[lane][step+1]~=nil then hits[lane]=overrides[lane][step+1] end
            end
            if hits[1] then add(1,36,beat,.20,step%4==0 and 110 or 86) end
            if hits[2] then add(2,38,beat,.15,104) end
            if hits[3] then add(3,42,beat,.08,step%4==2 and 83 or 58) end
            if lift and bar%2==1 and step==15 then add(2,38,beat,.1,59) end
        end
        for _,offset in ipairs({0,1.5,2.5,3.25}) do
            if not intro or offset==0 or offset==2.5 then
                add(4,root-12+(offset==3.25 and 12 or 0),bar*4+offset,offset==0 and 1.1 or .43,100)
            end
        end
        for _,pitch in ipairs(chord) do add(5,pitch,bar*4,3.75,intro and 65 or 77) end
        if not intro or bar%2==1 then
            for step=0,7 do
                if rand()<.45+o.density*.5 then
                    local pitch=chord[(step+(bar%2)*2)%4+1]+12
                    add(6,pitch,bar*4+step*.5+(step%2)*o.swing*.25,.27,76+step%3*6)
                end
            end
        end
    end
    for _,lane in ipairs(plan.lanes) do table.sort(lane.notes,function(a,b) return a.start<b.start end) end
    return plan
end
function M.verify(plan)
    local count=0
    for _,lane in ipairs(plan.lanes) do
        local seen={}
        for _,n in ipairs(lane.notes) do
            assert(not seen[n.id],"Duplicate note ID"); seen[n.id]=true
            assert(n.pitch>=0 and n.pitch<=127,"Pitch outside MIDI range")
            assert(n.velocity>=1 and n.velocity<=127,"Invalid velocity")
            assert(n.start>=0 and n.length>0 and n.start+n.length<=plan.beats+1e-6,"Note outside the arrangement")
            count=count+1
        end
    end
    assert(count==plan.count and count>0,"Note count mismatch")
    return count
end
return M
