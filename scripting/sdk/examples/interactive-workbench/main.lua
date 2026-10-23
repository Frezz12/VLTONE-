-- An ordinary Lua table describes native Qt controls; callbacks run in the worker.
local point = {x=0.5, y=0.5}
local files = {}
local function message(text) vlt.ui.set{id="status", text=text} end
local function draw()
    local size = vlt.ui.get{id="pad"}
    local x, y = point.x*size.width, point.y*size.height
    vlt.ui.set{id="pad", drawing={
        {type="line", x=0, y=y, x2=size.width, y2=y, color="#6e879c"},
        {type="line", x=x, y=0, x2=x, y2=size.height, color="#6e879c"},
        {type="ellipse", x=x-7, y=y-7, w=14, h=14, fill=true, color="#56b5cc"},
        {type="text", x=12, y=22, text="Drag or use arrow keys"}
    }}
end
local function refresh()
    local page = vlt.project.files{offset=0, limit=256}
    files = page.files
    local rows = vlt.array({})
    for _, file in ipairs(files) do
        rows[#rows+1] = {file.name, tostring(file.bytes)}
    end
    vlt.ui.set{id="media", rows=rows}
    message(#files.." of "..page.total.." project files (first page)")
end
local function navigate(view)
    local ok, err = pcall(function() vlt.workspace.open{view=view} end)
    message(ok and ("Opened "..view) or tostring(err))
end
vlt.ui.window{id="workbench", title="Project workbench", dock="right", width=620, height=520,
children={
    {type="tabs", children={
        {type="column", title="Project media", children={
            {type="row", children={
                {type="button", text="Refresh", onClick=refresh},
                {type="button", text="Mixer", onClick=function() navigate("mixer") end},
                {type="button", text="Browser", onClick=function() navigate("browser") end}
            }},
            {type="table", id="media", columns={"File", "Bytes"}, rows=vlt.array({}),
                minHeight=180, onClick=function(e)
                    local file=files[e.value.row]
                    if file then message(file.path) end
                end},
            {type="textEdit", id="notes", readOnly=true, height=70,
                value="Linked project media is readable automatically. Click a row for its full path."}
        }},
        {type="scroll", title="Custom widget", children={
            {type="group", text="XY pad", children={
                {type="canvas", id="pad", label="XY pad; drag or use arrow keys", minHeight=220,
                    onPointer=function(e)
                        if e.value.phase=="down" or (e.value.phase=="move" and e.value.buttons~=0) then
                            point.x=math.max(0,math.min(1,e.value.nx))
                            point.y=math.max(0,math.min(1,e.value.ny))
                            draw()
                        end
                    end,
                    onKey=function(e)
                        local delta=0.02
                        if e.value.key=="Left" then point.x=math.max(0,point.x-delta) end
                        if e.value.key=="Right" then point.x=math.min(1,point.x+delta) end
                        if e.value.key=="Up" then point.y=math.max(0,point.y-delta) end
                        if e.value.key=="Down" then point.y=math.min(1,point.y+delta) end
                        draw()
                    end,
                    onResize=draw},
                {type="grid", columns=2, children={
                    {type="number", label="Grid example", min=1,max=16,value=4},
                    {type="choice", label="Mode", items={"Free", "Snap"}}
                }}
            }}
        }}
    }},
    {type="label",id="status",text="Ready"}
}}
refresh()
draw()
