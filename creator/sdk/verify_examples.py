"""Build SDK examples with the actual Creator service and optionally run DSP.

Uses only Python's standard library. This is a documentation verification tool,
not a requirement for module authors working in the Creator window.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def service(tools, job, request):
    request = dict(request, tools=str(tools))
    write_json(job / "request.json", request)
    executable = tools / ("daw_creator_compiler.exe" if os.name == "nt" else "daw_creator_compiler")
    subprocess.run([str(executable), str(job / "request.json"), str(job / "response.json")],
                   check=True, timeout=120, capture_output=True,
                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    response = json.loads((job / "response.json").read_text(encoding="utf-8"))
    if not response.get("ok"):
        raise RuntimeError(json.dumps(response, ensure_ascii=False, indent=2))
    return response


def control(key, low, high, default, unit="", logarithmic=False):
    return {"id": key, "name": key, "unit": unit, "min": float(low),
            "max": float(high), "default": float(default), "log": logarithmic,
            "style": "", "bindings": []}


def wire(source, destination, source_port="out", destination_port="in"):
    return {"from": source, "to": destination,
            "fromPort": source_port, "toPort": destination_port}


def node(key, kind, **parameters):
    return {"id": key, "type": kind, "version": 1, "parameters": parameters}


def cases():
    drive = control("drive", .1, 6, 1.5)
    blend = control("mix", 0, 1, .3)
    threshold = control("threshold", 0, 1, .1)
    return [
        ("Gain", [control("gain", 0, 2, 1)], [], [], "out"),
        ("LocalHelpers", [drive, blend], [], [], "out"),
        ("OnePole", [control("cutoff", 20, 20000, 1500, "Hz", True)], [], [], "out"),
        ("MultiOutput", [threshold], [], [], "audio"),
        ("Delay", [control("time", 1, 500, 120, "ms", True),
                   control("mix", 0, 1, .25)], [], [], "out"),
        ("Chorus", [control("amount", 0, 100, 35, "%"),
                    control("rate", .05, 1.5, .25, "Hz", True)], [], [], "out"),
        ("ControlLFO", [control("rate", .01, 20, 1, "Hz", True),
                        control("depth", 0, 1, .5)], [], [], "out"),
        ("SeededNoise", [control("level", 0, .2, .03)], [], [], "out"),
        ("CallSaturate", [drive], [("sat", "Saturate", "saturate")],
         [wire("sat", "cpp", "function", "saturate")], "out"),
        ("TwoFilters", [control("cutoff", 20, 5000, 500, "Hz", True),
                        control("mix", 0, 1, .5)], [("filter", "OnePole", "process")],
         [wire("filter", "cpp", "function", "filter_a"),
          wire("filter", "cpp", "function", "filter_b")], "out"),
        ("NestedCall", [drive, blend], [("middle", "CallSaturate", "process"),
                                       ("sat", "Saturate", "saturate")],
         [wire("sat", "middle", "function", "saturate"),
          wire("middle", "cpp", "function", "processor")], "out"),
        ("CallMultiOutput", [threshold], [("probe", "MultiOutput", "process")],
         [wire("probe", "cpp", "function", "analyze")], "out"),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", required=True, type=Path, help="Installed CreatorTools directory")
    parser.add_argument("--validator", type=Path, help="Optional creator_graph_test executable")
    parser.add_argument("--out", type=Path, default=ROOT / "examples" / "projects")
    options = parser.parse_args()
    tools = options.tools.resolve()
    report = []
    with tempfile.TemporaryDirectory(prefix="vlt-sdk-docs-") as temporary:
        job = Path(temporary)
        functions = {}

        def analyze(name, entry="process"):
            key = (name, entry)
            if key not in functions:
                function = {"sdk": 1, "entry": entry, "inputs": [], "outputs": [],
                            "source": (ROOT / "examples" / f"{name}.cpp").read_text(encoding="utf-8")}
                functions[key] = service(tools, job, {"action": "analyze", "function": function})["function"]
            return functions[key]

        for name, controls, helpers, connections, output_port in cases():
            cpp = dict(node("cpp", "cpp_function"), function=analyze(name))
            nodes = [node("in", "input"), node("out", "output"), cpp, node("ui", "interface")]
            wires = list(connections)
            if any(p["type"] == "audio" for p in cpp["function"]["inputs"]):
                wires.append(wire("in", "cpp", "out", "input"))
            for parameter in controls:
                wires.append(wire("ui", "cpp", parameter["id"], parameter["id"]))
            for key, file_name, entry in helpers:
                nodes.append(dict(node(key, "cpp_function"), function=analyze(file_name, entry)))
            if name == "ControlLFO":
                nodes.append(node("gain", "gain", gain=1.0))
                wires += [wire("in", "gain"), wire("cpp", "gain", "out", "gain"), wire("gain", "out")]
            else:
                wires.append(wire("cpp", "out", output_port, "in"))

            unit = {"functions": [], "bindings": []}
            for index, item in enumerate(nodes):
                if "function" in item:
                    unit["functions"].append({"id": item["id"], "index": index,
                                              "scheduled": item["id"] == "cpp",
                                              "function": item["function"]})
            for edge in wires:
                if edge["fromPort"] == "function":
                    unit["bindings"].append({"from": edge["from"], "to": edge["to"], "port": edge["toPort"]})
            reply = service(tools, job, dict(unit, action="compile"))
            definition = {"id": f"creator.sdk.{name}", "name": f"SDK {name}", "version": 4,
                          "nodes": nodes, "connections": wires, "controls": controls,
                          "modes": [], "defaultMode": "",
                          "appearance": {"theme": "studio", "controlStyle": "machined",
                                         "backgroundColor": "", "backgroundImage": ""},
                          "code": {"abi": 1, "wasm": reply["wasm"],
                                   "sourceHash": hashlib.sha256(canonical(unit).encode()).hexdigest()}}
            output = options.out.resolve()
            module_path = output / f"{name}.vltmini"
            write_json(module_path, {"format": "vltmini", "version": 4, "definition": definition})
            positions = {"in": [20, 40], "cpp": [390, 40], "out": [820, 40], "ui": [20, 310]}
            for i, extra in enumerate(nodes[4:]):
                positions[extra["id"]] = [390 + (i % 2) * 390, 460 + (i // 2) * 350]
            write_json(output / f"{name}.vltcreator", {
                "format": "vltcreator", "version": 2, "name": f"SDK {name}",
                "definition": definition, "activeMode": "", "positions": {"": positions},
                "viewports": {}, "codeNode": "cpp", "codeCursors": {}})
            validation = "not run"
            if options.validator:
                env = dict(os.environ, VLT_CREATOR_TOOLS=str(tools))
                result = subprocess.run([str(options.validator.resolve()), "--validate", str(module_path)],
                                        env=env, capture_output=True, text=True, timeout=120,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                if result.returncode or "FAIL" in result.stdout:
                    raise RuntimeError(f"{name}:\n{result.stdout}\n{result.stderr}")
                validation = "passed at 48000 Hz, stereo"
            print(f"PASS {name}: analyze, WebAssembly, DSP {validation}", flush=True)
            report.append({"name": name, "analysis": True, "wasm": True, "runtime": validation})

        # The local helper is also valid for the actual Extract function action.
        extracted = service(tools, job, {"action": "extract", "function": analyze("LocalHelpers"), "name": "saturate"})
        print("PASS Extract function: LocalHelpers.saturate", flush=True)
        write_json(options.out / "verification.json", {"sdk": 1, "examples": report,
                   "extraction": bool(extracted["ok"]), "platform": os.name})


if __name__ == "__main__":
    main()
