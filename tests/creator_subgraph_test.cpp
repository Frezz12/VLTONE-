#include "CreatorProject.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>
#include <nlohmann/json.hpp>
using namespace daw::plugins::mini;
static std::vector<float> response(const MiniModuleDefinition &definition) {
  MiniModuleInstance fx;if(!fx.configure(definition)||!fx.activate({48000,64}))return {};
  fx.startProcessing();std::vector<float> input(64),output(64),right(64);input[0]=2;input[1]=-.5f;
  const float *in[]{input.data(),input.data()};float *out[]{output.data(),right.data()};
  daw::plugins::PluginProcessContext ctx;ctx.inputs=in;ctx.outputs=out;ctx.frames=64;ctx.inputChannels=ctx.outputChannels=2;
  return fx.process(ctx)==daw::plugins::PluginProcessDisposition::Error?std::vector<float>{}:output;
}
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv); int failures=0;
  const auto check=[&](bool ok,const char *name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);failures+=!ok;};
  auto p=ui::CreatorProject::create("Test","Group");
  p.definition.nodes.push_back(makeNode("audio_scale","scale")); p.definition.nodes.push_back(makeNode("hard_clip","clip"));
  p.definition.connections={{"input","scale","out","in"},{"scale","clip","out","in"},{"clip","output","out","in"}};
  p.positions[{}]["scale"]={300,100};p.positions[{}]["clip"]={600,100};
  const auto original=p;QString error;
  const auto id=p.pack({"scale","clip"},"My sound",error);
  check(!id.isEmpty() && error.isEmpty() && validate(p.definition).empty(),"selection becomes typed custom node");
  check(p.definition.subgraphs.size()==1 && p.definition.subgraphs[0].inputs.size()==1 && p.definition.subgraphs[0].outputs.size()==1,"crossing wires become stable ports");
  check(!response(original.definition).empty()&&response(p.definition)==response(original.definition),"packing preserves rendered audio exactly");
  const auto port=p.definition.subgraphs[0].inputs[0].id;
  p.definition.subgraphs[0].inputs[0].name="Renamed input";
  auto renamed=p.definition;renamed.subgraphs[0].name="Renamed group";renamed.nodes.back().label="New label";
  check(sameAudioGraph(p.definition,{},renamed,{}),"cosmetic custom-node edits do not invalidate DSP or Freeze");
  check(p.definition.subgraphs[0].inputs[0].id==port && validate(p.definition).empty(),"renaming keeps port identity and wires");
  auto instance=p.definition.nodes.back();instance.id="second";p.definition.nodes.push_back(instance);
  check(p.makeIndependent("second",error) && p.definition.subgraphs.size()==2 && p.definition.nodes.back().subgraph!=instance.subgraph,"independent copy duplicates definition");
  p.graphPath.push_back(QString::fromStdString(p.definition.subgraphs.front().id));
  auto inner=p.graph();check(inner.nodes.size()==4,"enter custom graph");
  inner.nodes[0].label="Inside";p.setGraph(inner);
  QTemporaryDir temp;check(p.save(temp.filePath("example.vltcreator"),error),"nested project saved");
  ui::CreatorProject reopened;check(ui::CreatorProject::open(temp.filePath("example.vltcreator"),reopened,error)&&reopened==p,"nested graph, navigation and layouts roundtrip");
  p.graphPath.clear();check(p.unpack(id,error)&&validate(p.definition).empty(),"custom node expands into graph");
  check(response(p.definition)==response(original.definition),"unpacking preserves rendered audio exactly");
  MiniModuleDefinition flat;std::string why;check(expandSubgraphs(p.definition,flat,why)&&flat.nodes.size()>=original.definition.nodes.size(),"expanded graph keeps executable nodes");
  auto missing=toJson(original.definition);missing["nodes"][0]["type"]="future_node";missing["nodes"][0]["future_property"]={{"payload",123}};
  auto future=fromJson(missing);check(!validate(future).empty(),"unknown node never executes");
  check(toJson(future)==missing,"unknown node data roundtrips losslessly");
  auto random=ui::CreatorProject::create("Random","Random");random.definition.nodes.push_back(makeNode("noise","noise"));
  random.definition.connections={{"noise","output","out","in"}};const auto noise=response(random.definition);
  const auto noiseGroup=random.pack({"noise"},"Noise",error);
  check(!noiseGroup.isEmpty()&&response(random.definition)==noise,"packing retains deterministic generator state identity");
  check(random.unpack(noiseGroup,error)&&response(random.definition)==noise,"unpacking retains deterministic generator state identity");
  return failures?1:0;
}
