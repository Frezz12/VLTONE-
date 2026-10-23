-- Open in Creator and choose Preview. This example does not modify your project.
local palettes = {
    {background="#121922", surface="#1d2936", field="#101720", text="#edf4fa",
     muted="#9aacbc", accent="#70e2bb", onAccent="#102820", border="#344658"},
    {background="#f3f1eb", surface="#ffffff", field="#e9eee9", text="#20352e",
     muted="#52685e", accent="#226c52", onAccent="#ffffff", border="#c8d6cf"}
}
local paletteIndex, amount = 1, 0.6
local function draw()
    local size = vlt.ui.get{id="shape"}
    local color = palettes[paletteIndex].accent
    local shapes = vlt.array({})
    for i=0,47 do
        local x = 16+i*(size.width-32)/48
        local h = (0.16+math.abs(math.sin(i*0.43))*amount)*(size.height-32)
        shapes[#shapes+1] = {type="rect",x=x,y=(size.height-h)/2,w=math.max(2,(size.width-32)/48-3),
                            h=h,fill=true,color=color}
    end
    vlt.ui.set{id="shape",drawing=shapes}
end
local function setAmount(value)
    amount = math.max(0,math.min(1,value))
    vlt.ui.set{id="amount",value=amount}
    vlt.ui.set{id="status",text=string.format("Intensity · %d%%",math.floor(amount*100+0.5))}
    draw()
end
vlt.ui.window{id="design",title="Shape studio",width=640,height=560,
 theme=palettes[paletteIndex],style={padding=24,fontSize=13},styles={
    title={fontSize=28,fontWeight=700,color="$text"},
    eyebrow={fontSize=11,fontWeight=600,color="$accent"},
    muted={color="$muted"},
    card={background="$surface",borderColor="$border",borderWidth=1,radius=14,padding=18},
    primary={background="$accent",color="$onAccent",borderWidth=0,radius=8,padding=10,fontWeight=600,
             hover={background="$text",color="$background"},pressed={background="$surface",color="$text"}},
    secondary={background="$surface",color="$text",borderColor="$border",borderWidth=1,radius=8,padding=10,
               hover={borderColor="$accent"}}
 },children={
    {type="label",text="VLTONE  /  SCRIPT DESIGN",style="eyebrow",wrap=false},
    {type="row",children={
        {type="label",text="Shape studio",style="title",stretch=1,wrap=false},
        {type="button",id="palette",text="Switch palette",style="secondary",onClick=function()
            paletteIndex = 3-paletteIndex
            vlt.ui.set{id="design",theme=palettes[paletteIndex]}
            draw()
        end}
    }},
    {type="label",text="Your palette. Your layout. Native keyboard controls.",style="muted"},
    {type="column",style="card",stretch=1,spacing=14,children={
        {type="row",children={
            {type="label",text="PREVIEW",style="eyebrow",stretch=1},
            {type="label",id="status",text="Intensity · 60%",style="muted",align="right"}
        }},
        {type="canvas",id="shape",label="Pattern intensity: drag or press arrow keys",minHeight=130,stretch=1,
         style={background="$field",radius=10},onResize=draw,
         onPointer=function(e)
             if e.value.phase=="down" or (e.value.phase=="move" and e.value.buttons~=0) then setAmount(e.value.nx) end
         end,
         onKey=function(e)
             if e.value.key=="Left" then setAmount(amount-0.05) end
             if e.value.key=="Right" then setAmount(amount+0.05) end
         end},
        {type="slider",id="amount",label="Intensity",min=0,max=1,value=amount,
         onChange=function(e) setAmount(e.value) end}
    }},
    {type="row",spacing=12,children={
        {type="column",style="card",stretch=1,spacing=8,children={
            {type="label",text="PRESET NAME",style="eyebrow"},
            {type="field",id="name",value="Evening pulse",style={radius=6,padding=8}}
        }},
        {type="column",style="card",stretch=1,spacing=8,children={
            {type="label",text="LIVE EDITING",style="eyebrow"},
            {type="label",text="Switch colors without losing your input.",style="muted"}
        }}
    }},
    {type="row",children={
        {type="label",id="message",text="Try the pad with your mouse or keyboard.",style="muted",stretch=1},
        {type="button",text="Reset",style="secondary",onClick=function() setAmount(0.6) end},
        {type="button",text="Use preset",style="primary",onClick=function()
            vlt.ui.set{id="message",text="Selected: "..vlt.ui.get{id="name"}.value}
        end}
    }}
 }}
draw()
