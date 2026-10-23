"""Real disposable-worker protocol tests; no GUI, external Lua or Python packages."""
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

worker = pathlib.Path(sys.argv[1]).resolve()

def read(process):
    header = process.stdout.read(8)
    assert len(header) == 8, (process.wait(), process.stderr.read().decode(errors="replace"))
    size, binary = struct.unpack("<II", header)
    assert 0 < size <= 2 * 1024 * 1024 and binary <= 1024 * 1024
    return json.loads(process.stdout.read(size)), process.stdout.read(binary)

def send(process, message, binary=b""):
    data = json.dumps(message).encode()
    process.stdin.write(struct.pack("<II", len(data), len(binary)) + data + binary)
    process.stdin.flush()

def start(source):
    p = subprocess.Popen([str(worker)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    assert read(p)[0] == {"type": "ready", "protocol": 1}
    send(p, {"type": "run", "entry": "main.lua", "sources": {"main.lua": source}})
    return p

def stop(p):
    p.kill()
    p.communicate(timeout=1)

catalog = json.loads(subprocess.check_output([str(worker), "--sdk"]))
assert catalog["sdk"] == 1 and catalog["lua"] == "5.5.1"
with tempfile.TemporaryDirectory() as tmp:
    source = pathlib.Path(tmp) / "test.lua"
    source.write_text('error("must not execute during validation")')
    assert subprocess.run([str(worker), "--check", str(source)]).returncode == 0
    source.write_text("local = broken")
    assert subprocess.run([str(worker), "--check", str(source)], capture_output=True).returncode != 0

p = start('assert(io==nil and os==nil and package==nil and debug==nil and loadfile==nil)\nreturn 42')
assert read(p)[0] == {"type": "idle", "value": 42}
send(p, {"type": "reset"})
assert read(p)[0]["type"] == "ready"
send(p, {"type": "run", "entry": "main.lua", "sources": {"main.lua": "assert(secret==nil); secret=1; return vlt.sdk"}})
assert read(p)[0]["value"] == 1
stop(p)

p = start('local co=coroutine.create(function() local c=vlt.context.get(); vlt.log{text=c.project}; return c.project end); local ok,value=coroutine.resume(co); assert(ok,value); return value')
call, _ = read(p)
assert call["method"] == "context.get" and call["arguments"] == {}, call
send(p, {"type": "reply", "id": call["id"], "ok": True, "value": {"project": "coroutine-project"}})
call, _ = read(p)
assert call.get("method") == "log" and call["arguments"] == {"text": "coroutine-project"}, call
send(p, {"type": "reply", "id": call["id"], "ok": True, "value": True})
assert read(p)[0] == {"type": "idle", "value": "coroutine-project"}
stop(p)

p = start('local c=vlt.context.get(); return c.target.clip')
call, _ = read(p)
assert call["method"] == "context.get"
send(p, {"type": "reply", "id": call["id"], "ok": True, "value": {"target": {"clip": "clicked-not-selected"}}})
assert read(p)[0]["value"] == "clicked-not-selected"
stop(p)

p = start('vlt.events.on{event="selection", callback=function(e) print(e.value) end}')
call, _ = read(p)
callback = call["arguments"]["callback"]["$callback"]
send(p, {"type": "reply", "id": call["id"], "ok": True, "value": "sub"})
assert read(p)[0]["type"] == "idle"
send(p, {"type": "event", "callback": callback, "value": {"value": "changed"}})
assert read(p)[0] == {"type": "log", "text": "changed"}
assert read(p)[0]["type"] == "idle"
stop(p)

# Unsubscribing must release the registry reference and its captured data. Without
# host-to-worker callback ownership updates this exceeds the 64 MiB Lua limit.
p = start('for i=1,48 do local payload=string.rep("x",2*1024*1024); '
          'local id=vlt.events.on{event="selection",callback=function() return #payload end}; '
          'vlt.events.off{id=id} end; return true')
for index in range(48):
    call, _ = read(p)
    assert call.get("method") == "events.on", call
    callback = call["arguments"]["callback"]["$callback"]
    send(p, {"type": "reply", "id": call["id"], "ok": True, "value": str(index),
             "callbacks": [callback]})
    call, _ = read(p)
    assert call.get("method") == "events.off", call
    send(p, {"type": "reply", "id": call["id"], "ok": True, "value": True,
             "callbacks": []})
assert read(p)[0] == {"type": "idle", "value": True}
stop(p)

p = start('local ok=pcall(function() vlt.transport.set{playing=true} end); return not ok')
call, _ = read(p)
send(p, {"type": "reply", "id": call["id"], "ok": False, "error": "Permission denied"})
assert read(p)[0] == {"type": "idle", "value": True}
stop(p)

# Lua / always yields a float; whole-valued results must keep integer SDK arguments.
p = start('return {frame=32768/2,frames=256/2,fraction=1/2,overflow=2.0^63}')
value = read(p)[0]["value"]
assert type(value["frame"]) is int and value["frame"] == 16384, value
assert type(value["frames"]) is int and value["frames"] == 128, value
assert value["fraction"] == 0.5 and type(value["fraction"]) is float, value
assert type(value["overflow"]) is float, value
stop(p)

p = start('return vlt.array({})')
assert read(p)[0] == {"type": "idle", "value": []}
stop(p)

# PCM storage must count against the Lua allocator, not grow outside its limit.
p = start('local blocks={} for i=1,100 do local info,pcm=vlt.audio.read{id="input",frame=0,frames=65536}; blocks[i]=pcm end')
blocks = 0
while True:
    call, _ = read(p)
    if call["type"] == "error":
        assert "memory" in call["error"].lower(), call
        break
    assert call["method"] == "audio.read"
    blocks += 1
    send(p, {"type": "reply", "id": call["id"], "ok": True, "value": {"frames": 65536}}, bytes(1024 * 1024))
assert 32 <= blocks <= 65, blocks
stop(p)

# Many aliases of one small Lua string must not allocate a huge host JSON tree.
p = start('local s=string.rep("x",65536); local t={} for i=1,1000 do t[i]=s end; return t')
assert read(p)[0]["type"] == "error"
stop(p)

p = start('while true do end')
begin = time.monotonic()
assert read(p)[0]["type"] == "error"
assert time.monotonic() - begin < 4
stop(p)

p = start('local t={} while true do t[#t+1]=string.rep("x",1024*1024) end')
assert read(p)[0]["type"] == "error"
stop(p)

p = start('return require("native")')
assert read(p)[0]["type"] == "error"
stop(p)

p = start('while true do end')
begin = time.monotonic()
stop(p)
assert time.monotonic() - begin < 1
print("PASS: syntax-only validation, sandbox, isolation/reset, IPC, callbacks, timeout, memory limit, stop < 1s")
