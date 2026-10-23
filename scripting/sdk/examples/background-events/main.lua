local previous=nil
vlt.events.on{event="transport", callback=function(event)
    if previous~=event.playing then
        print(event.playing and "Playback started" or "Playback stopped", "beat", event.beat)
        previous=event.playing
    end
end}
vlt.events.on{event="project", callback=function(event) print("Project revision", event.revision) end}
print("Watching the pinned project. Stop this instance in Script manager.")
-- Installation does not start this tool; autostart is a separate user setting.
