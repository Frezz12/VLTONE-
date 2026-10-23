local function draw(beat)
    local shapes=vlt.array({})
    for i=0,16 do
        shapes[#shapes+1]={type="line", x=20+i*24, y=20, x2=20+i*24, y2=(i%4==0) and 78 or 50}
    end
    shapes[#shapes+1]={type="rect", x=20+(beat%16)*24, y=20, w=3, h=65, color="#579ce8", fill=true}
    shapes[#shapes+1]={type="text", x=20, y=110, text=string.format("Beat %.2f",beat)}
    return shapes
end
vlt.ui.window{id="ruler", title="Beat ruler", width=460, height=240, children={
    {type="label", text="Drawing follows transport in this window's project."},
    {type="canvas", id="rulerDrawing", drawing=draw(vlt.transport.get().beat)}
}}
vlt.events.on{event="transport", callback=function(event)
    vlt.ui.set{id="rulerDrawing", drawing=draw(event.beat)}
end}
