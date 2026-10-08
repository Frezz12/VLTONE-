"""Regenerate the portable, code-free Creator 3 teaching projects."""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parent / "examples" / "nodes"

def node(kind, key, **params):
    return {"id": key, "type": kind, "version": 1, "parameters": params}

def edge(source, target, output="out", port="in"):
    return {"from": source, "to": target, "fromPort": output, "toPort": port}

def control(key, name, minimum, maximum, default, unit="", logarithmic=False):
    return {"id": key, "name": name, "min": minimum, "max": maximum,
            "default": default, "unit": unit, "log": logarithmic, "bindings": []}

def graph(name, nodes, connections, controls=(), subgraphs=()):
    return {"id": "creator.nodes." + name, "name": name, "version": 5,
            "nodes": [node("input", "input"), node("output", "output"), node("interface", "interface"), *nodes],
            "connections": connections, "controls": list(controls), "subgraphs": list(subgraphs)}

def save(name, definition, positions=None):
    ROOT.mkdir(parents=True, exist_ok=True)
    if positions is None:
        positions = {n["id"]: [40 + (i % 4) * 310, 60 + (i // 4) * 270]
                     for i, n in enumerate(definition["nodes"])}
    layout = {"": positions}
    for group in definition["subgraphs"]:
        layout["node/" + group["id"]] = {n["id"]: [40 + (i % 4) * 290, 60 + (i // 4) * 230]
                                         for i, n in enumerate(group["nodes"])}
    project = {"format": "vltcreator", "version": 3, "name": name,
               "activeMode": "", "graphPath": [], "definition": definition,
               "positions": layout, "viewports": {"": {"center": [630, 380], "zoom": .8}}}
    mini = {"format": "vltmini", "version": 5, "definition": definition}
    for suffix, value in ((".vltcreator", project), (".vltmini", mini)):
        (ROOT / (name + suffix)).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

def main():
    nodes = [node("peak", "peak"), node("linear_to_db", "db"), node("subtract", "over"),
             node("max", "knee", b=0), node("attack_release", "envelope", attack=8, release=120),
             node("divide", "reciprocal", a=1), node("subtract", "slope", b=1),
             node("multiply", "reduction"), node("db_to_linear", "gain"), node("audio_scale", "vca")]
    connections = [edge("input", "peak"), edge("peak", "db", port="value"), edge("db", "over", port="a"),
                   edge("interface", "over", "threshold", "b"), edge("over", "knee", port="a"),
                   edge("knee", "envelope", port="value"), edge("interface", "reciprocal", "ratio", "b"),
                   edge("reciprocal", "slope", port="a"), edge("envelope", "reduction", port="a"),
                   edge("slope", "reduction", port="b"), edge("reduction", "gain", port="value"),
                   edge("gain", "vca", port="gain"), edge("input", "vca"), edge("vca", "output")]
    save("NodeCompressor", graph("NodeCompressor", nodes, connections,
         [control("threshold", "Threshold", -60, 0, -18, "dB"), control("ratio", "Ratio", 1, 20, 4)]))

    eq = graph("NodeEqualizer", [node("biquad_coefficients", "coefficients", frequency=1200, q=.70710678, gain=6, type=5), node("biquad", "filter")],
               [edge("input", "filter"), edge("coefficients", "filter", port="coefficients"),
                edge("interface", "coefficients", "frequency", "frequency"), edge("interface", "coefficients", "gain", "gain"), edge("filter", "output")],
               [control("frequency", "Frequency", 30, 18000, 1200, "Hz", True), control("gain", "Gain", -24, 24, 6, "dB")])
    save("NodeEqualizer", eq)

    def boundary(kind, key, value_type):
        return dict(node(kind, key), port=key, valueType=value_type)
    group = {"id": "saturator.4x", "name": "Stereo waveshaper", "version": 5, "oversampling": 4,
             "inputs": [{"id": "signal", "name": "Signal", "type": "audio"}, {"id": "drive", "name": "Drive", "type": "number", "min": 1, "max": 12, "default": 2}],
             "outputs": [{"id": "signal", "name": "Signal", "type": "audio"}],
             "nodes": [boundary("subgraph_input", "signal", "audio"), boundary("subgraph_input", "drive", "number"),
                       node("audio_scale", "pre"), node("stereo_split", "split"), node("tanh", "left"), node("tanh", "right"),
                       node("stereo_join", "join"), dict(boundary("subgraph_output", "signal", "audio"), id="result")],
             "connections": [edge("signal", "pre"), edge("drive", "pre", port="gain"), edge("pre", "split"),
                             edge("split", "left", "left", "value"), edge("split", "right", "right", "value"),
                             edge("left", "join", port="left"), edge("right", "join", port="right"), edge("join", "result")]}
    sat = graph("NodeSaturator4x", [dict(node("subgraph", "saturator"), subgraph=group["id"]), node("audio_scale", "output_gain")],
                [edge("input", "saturator", port="signal"), edge("interface", "saturator", "drive", "drive"),
                 edge("saturator", "output_gain", "signal"), edge("interface", "output_gain", "level", "gain"), edge("output_gain", "output")],
                [control("drive", "Drive", 1, 12, 2), control("level", "Level", 0, 1, .65)], [group])
    save("NodeSaturator4x", sat)

    delay = graph("NodeFeedbackDelay", [node("delay_buffer", "buffer", maximum=2000), node("delay_read", "tap", time=280),
                 node("audio_scale", "feedback", gain=.4), node("audio_add", "write"), node("audio_add", "mix")],
                 [edge("input", "write", port="a"), edge("tap", "feedback"), edge("feedback", "write", port="b"),
                  edge("write", "buffer"), edge("buffer", "tap", port="buffer"), edge("interface", "tap", "time", "time"),
                  edge("interface", "feedback", "feedback", "gain"), edge("input", "mix", port="a"), edge("tap", "mix", port="b"), edge("mix", "output")],
                 [control("time", "Time", 1, 2000, 280, "ms"), control("feedback", "Feedback", 0, .9, .4)])
    save("NodeFeedbackDelay", delay)
    # A library node carries the complete dependency set and its own layouts.
    project = json.loads((ROOT / "NodeSaturator4x.vltcreator").read_text(encoding="utf-8"))
    project.update(format="vltnode", version=1, root=group["id"])
    project["definition"] = dict(sat, nodes=[], connections=[], controls=[])
    (ROOT / "StereoWaveshaper.vltnode").write_text(json.dumps(project, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Generated 4 projects, 4 mini modules and 1 reusable node in {ROOT}")

if __name__ == "__main__":
    main()
