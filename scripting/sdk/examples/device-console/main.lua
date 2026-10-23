local project=vlt.project.get()
local children=vlt.array({})
local count=0
for _, track in ipairs(project.tracks) do
    for _, device in ipairs(track.devices) do
        if count<12 then
            local controls=vlt.array({})
            for _, parameter in ipairs(vlt.parameters.list{track=track.id, device=device.id}) do
                if not parameter.readOnly and #controls<3 and count<12 then
                    controls[#controls+1]={
                        type="knob", id="parameter"..count, label=parameter.name,
                        bind={track=track.id, device=device.id, parameter=parameter.id, identity=parameter.identity}
                    }
                    count=count+1
                end
            end
            if #controls>0 then
                children[#children+1]={type="label", text=track.name.." / "..device.name}
                children[#children+1]={type="row", children=controls}
            end
        end
    end
end
if #children==0 then children={{type="label", text="Add an instrument or effect, then reopen this console."}} end
vlt.ui.window{id="console", title="Device parameter console", width=620, height=640, children=children}
-- Native bindings synchronize both ways and commit one Undo at gesture end.
