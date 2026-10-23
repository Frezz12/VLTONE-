local music=require("music")
local count,variants=0,0
for _,bars in ipairs({4,8,16}) do
    for progression=1,3 do
        local o={bars=bars,tempo=112,seed=2718,root=45,progression=progression,swing=.14,density=.65,humanize=.016}
        local a,b=music.build(o),music.build(o)
        count=count+music.verify(a);variants=variants+1
        assert(a.count==b.count,"Determinism: count")
        for lane=1,6 do for i,note in ipairs(a.lanes[lane].notes) do
            local other=b.lanes[lane].notes[i]
            assert(note.start==other.start and note.pitch==other.pitch and note.velocity==other.velocity,"Determinism: content")
        end end
        local changed=music.build(o,{{[1]=false,[2]=true}})
        assert(changed.lanes[1].notes[1].start>=.24,"Grid overrides were ignored")
    end
end
print("Arrangement Lab: "..variants.." deterministic variants; "..count.." valid notes")
return {passed=true,variants=variants,notesChecked=count,projectUnchanged=true}
